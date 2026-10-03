#include "sentence_model.h"

#include "json.h"
#if defined(MSIME_NEURAL_AVX2)
#include "int8_linear_avx2.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace neural
{
namespace
{

// The exact GELU, matching torch.nn.functional.gelu with its default settings. The tanh
// approximation is a different function and the model was not trained with it.
float erf_approx(float x)
{
    // Abramowitz and Stegun 7.1.26; error stays below 1.5e-7 across the range.
    float sign = x < 0.0f ? -1.0f : 1.0f;
    x = std::fabs(x);
    float t = 1.0f / (1.0f + 0.327591100f * x);
    float poly = t * (0.254829600f + t * (-0.284496740f + t * (1.421413700f + t * (-1.453152000f + t * 1.061405400f))));
    return sign * (1.0f - poly * std::exp(-x * x));
}

float gelu(float x)
{
    constexpr float kInvSqrt2 = 0.70710678118654752440f;
    return 0.5f * x * (1.0f + erf_approx(x * kInvSqrt2));
}

// Four independent accumulators, not one.
//
// This is where essentially all of the model's time goes, so the shape of the loop matters. A single
// running sum makes every iteration wait on the previous add: the FP unit has a ~4-cycle add latency
// but can start one per cycle, so a serial chain leaves it idle three cycles out of four, and the
// compiler cannot fix it — reassociating floating-point addition changes the result, which /fp:precise
// forbids it from doing on its own. Splitting the sum into four lanes that only meet at the end is
// that same reassociation, written out in the source where it is allowed, and it also gives the
// vectorizer a shape it can fold into packed adds.
//
// The trade is that the result differs from a strict left-to-right sum in the last bits. That is fine
// here — these are log probabilities compared against each other, and the parity test against the
// per-character reference path has room to spare — but it does mean this is not bit-identical to a
// naive reference implementation.
float dot(const float *a, const float *b, std::size_t n)
{
    float s0 = 0.0f;
    float s1 = 0.0f;
    float s2 = 0.0f;
    float s3 = 0.0f;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4)
    {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    float total = (s0 + s1) + (s2 + s3);
    for (; i < n; ++i)
    {
        total += a[i] * b[i];
    }
    return total;
}

float dot_i8(const float *a, const std::int8_t *b, std::size_t n)
{
    float total = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
    {
        total += a[i] * static_cast<float>(b[i]);
    }
    return total;
}

// out[m][o] = dot(x[m], w[o]) + bias[o], both operands row-major.
//
// Output-major on purpose: the weights are far larger than cache (the desktop model's are ~25 MB
// total), so with the rows on the outside every input row restreams the entire matrix from memory
// and the whole forward pass runs at DRAM bandwidth. Hoisting the output loop reads each weight row
// once and reuses it across the batch, which is the reason a batch of candidates costs barely more
// than one.
void linear_into(const std::vector<float> &x, const Matrix &w, const std::vector<float> &bias, std::size_t inputs,
                 std::size_t outputs, std::vector<float> &out, std::vector<float> &scratch)
{
    std::size_t rows = inputs == 0 ? 0 : x.size() / inputs;
    out.resize(rows * outputs);
#if defined(MSIME_NEURAL_AVX2)
    static const bool use_avx2 = detail::supports_avx2_fma();
    if (w.quantized && use_avx2 && rows != 0 && inputs != 0)
    {
        detail::linear_int8_avx2(x.data(), w.ints.data(), w.scales.data(), bias.empty() ? nullptr : bias.data(), rows,
                                 inputs, outputs, out.data());
        return;
    }
#endif
    if (w.quantized && scratch.size() < inputs)
    {
        scratch.resize(inputs);
    }
    for (std::size_t o = 0; o < outputs; ++o)
    {
        // Widen the row's int8 once per output row instead of once per (output row, input row).
        // The conversion is what keeps the inner product from vectorizing, and across a batch it
        // would otherwise be repeated for every row of the batch.
        //
        // The per-row scale still multiplies the finished dot product rather than the weights, the
        // same way Matrix::dot_row does it: widening an int8 is exact, but folding the scale into
        // each weight beforehand would round every element.
        const float *weights;
        float scale = 1.0f;
        if (w.quantized)
        {
            const std::int8_t *source = &w.ints[o * inputs];
            for (std::size_t i = 0; i < inputs; ++i)
            {
                scratch[i] = static_cast<float>(source[i]);
            }
            weights = scratch.data();
            scale = w.scales[o];
        }
        else
        {
            weights = &w.floats[o * inputs];
        }

        const float b = bias.empty() ? 0.0f : bias[o];
        for (std::size_t row = 0; row < rows; ++row)
        {
            out[row * outputs + o] = dot(&x[row * inputs], weights, inputs) * scale + b;
        }
    }
}

void layer_norm_into(const float *x, std::size_t rows, const std::vector<float> &weight, const std::vector<float> &bias,
                     std::size_t width, std::vector<float> &out)
{
    out.resize(rows * width);
    for (std::size_t row = 0; row < rows; ++row)
    {
        const float *source = &x[row * width];
        float *target = &out[row * width];
        float mean = 0.0f;
        for (std::size_t i = 0; i < width; ++i)
        {
            mean += source[i];
        }
        mean /= static_cast<float>(width);
        float variance = 0.0f;
        for (std::size_t i = 0; i < width; ++i)
        {
            float d = source[i] - mean;
            variance += d * d;
        }
        variance /= static_cast<float>(width);
        float inverse = 1.0f / std::sqrt(variance + 1e-5f);
        for (std::size_t i = 0; i < width; ++i)
        {
            target[i] = (source[i] - mean) * inverse * weight[i] + bias[i];
        }
    }
}

void split_qkv(const std::vector<float> &qkv, std::size_t rows, std::size_t n_embd, std::vector<float> &q,
               std::vector<float> &k, std::vector<float> &v)
{
    q.resize(rows * n_embd);
    k.resize(rows * n_embd);
    v.resize(rows * n_embd);
    for (std::size_t row = 0; row < rows; ++row)
    {
        const float *source = &qkv[row * 3 * n_embd];
        std::memcpy(&q[row * n_embd], source, n_embd * sizeof(float));
        std::memcpy(&k[row * n_embd], source + n_embd, n_embd * sizeof(float));
        std::memcpy(&v[row * n_embd], source + 2 * n_embd, n_embd * sizeof(float));
    }
}

// IEEE 754 binary16 to binary32, with subnormal handling rather than flush-to-zero.
float half_to_f32(std::uint16_t bits)
{
    std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
    std::uint32_t exponent = static_cast<std::uint32_t>((bits >> 10) & 0x1fu);
    std::uint32_t mantissa = static_cast<std::uint32_t>(bits & 0x03ffu);
    std::uint32_t out;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            out = sign;
        }
        else
        {
            unsigned leading = 0;
            std::uint32_t m = mantissa;
            // Count leading zeros of a 32-bit value.
            while ((m & 0x80000000u) == 0)
            {
                m <<= 1;
                ++leading;
            }
            std::uint32_t exp = 134u - leading;
            std::uint32_t mant = (mantissa << (leading - 8)) & 0x007fffffu;
            out = sign | (exp << 23) | mant;
        }
    }
    else if (exponent == 0x1f)
    {
        out = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        out = sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
    }
    float result;
    std::memcpy(&result, &out, sizeof(result));
    return result;
}

// Decodes UTF-8 text into Unicode code points. Malformed bytes map to U+FFFD.
std::vector<char32_t> decode_utf8(const std::string &text)
{
    std::vector<char32_t> out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n)
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        char32_t code;
        std::size_t extra;
        if (c < 0x80)
        {
            code = c;
            extra = 0;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            code = c & 0x1F;
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            code = c & 0x0F;
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            code = c & 0x07;
            extra = 3;
        }
        else
        {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= n)
        {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (std::size_t j = 1; j <= extra; ++j)
        {
            unsigned char cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
            code = (code << 6) | (cc & 0x3F);
        }
        if (!ok)
        {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(code);
        i += extra + 1;
    }
    return out;
}

// A tensor descriptor from the safetensors header.
struct Entry
{
    std::string dtype;
    std::vector<std::size_t> shape;
    std::size_t off0 = 0;
    std::size_t off1 = 0;
};

std::vector<float> read_f32(const std::uint8_t *data, std::size_t data_len, const Entry &entry)
{
    if (entry.off1 > data_len || entry.off0 > entry.off1)
    {
        throw std::runtime_error("tensor offsets out of range");
    }
    std::size_t bytes = entry.off1 - entry.off0;
    std::size_t count = bytes / 4;
    std::vector<float> out(count);
    const std::uint8_t *p = data + entry.off0;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::memcpy(&out[i], p + i * 4, 4);
    }
    return out;
}

std::vector<float> read_f16(const std::uint8_t *data, std::size_t data_len, const Entry &entry)
{
    if (entry.off1 > data_len || entry.off0 > entry.off1)
    {
        throw std::runtime_error("tensor offsets out of range");
    }
    std::size_t bytes = entry.off1 - entry.off0;
    std::size_t count = bytes / 2;
    std::vector<float> out(count);
    const std::uint8_t *p = data + entry.off0;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::uint16_t bits;
        std::memcpy(&bits, p + i * 2, 2);
        out[i] = half_to_f32(bits);
    }
    return out;
}

std::size_t product(const std::vector<std::size_t> &shape)
{
    std::size_t total = 1;
    for (std::size_t d : shape)
    {
        total *= d;
    }
    return total;
}

} // namespace

#if defined(MSIME_NEURAL_AVX2)
namespace detail
{
bool supports_avx2_fma()
{
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, 0);
    if (info[0] < 7)
    {
        return false;
    }
    __cpuid(info, 1);
    constexpr int kFma = 1 << 12;
    constexpr int kOsxsave = 1 << 27;
    constexpr int kAvx = 1 << 28;
    if ((info[2] & (kFma | kOsxsave | kAvx)) != (kFma | kOsxsave | kAvx) || (_xgetbv(0) & 6) != 6)
    {
        return false;
    }
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#else
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
}
} // namespace detail
#endif

struct SentenceModel::Workspace
{
    std::vector<float> normed;
    std::vector<float> qkv;
    std::vector<float> q;
    std::vector<float> k;
    std::vector<float> v;
    std::vector<float> attended;
    std::vector<float> projected;
    std::vector<float> ff_hidden;
    std::vector<float> ff_out;
    std::vector<float> linear_scratch;
    std::vector<float> attention_scores;
};

float Matrix::dot_row(const float *x, std::size_t row, std::size_t width) const
{
    if (quantized)
    {
        return dot_i8(x, &ints[row * width], width) * scales[row];
    }
    return dot(x, &floats[row * width], width);
}

void Matrix::row_into(std::size_t index, std::size_t width, float *out) const
{
    if (quantized)
    {
        float scale = scales[index];
        const std::int8_t *p = &ints[index * width];
        for (std::size_t i = 0; i < width; ++i)
        {
            out[i] = static_cast<float>(p[i]) * scale;
        }
    }
    else
    {
        std::memcpy(out, &floats[index * width], width * sizeof(float));
    }
}

std::unique_ptr<SentenceModel> SentenceModel::load(const std::vector<std::uint8_t> &bytes)
{
    try
    {
        if (bytes.size() < 8)
        {
            return nullptr;
        }
        std::uint64_t header_len = 0;
        std::memcpy(&header_len, bytes.data(), 8);
        std::size_t header_end = 8 + static_cast<std::size_t>(header_len);
        if (header_len == 0 || header_end > bytes.size())
        {
            return nullptr;
        }
        std::string header_text(reinterpret_cast<const char *>(bytes.data() + 8), static_cast<std::size_t>(header_len));
        json::Value header = json::parse(header_text);
        if (!header.is_object())
        {
            return nullptr;
        }
        const std::uint8_t *data = bytes.data() + header_end;
        std::size_t data_len = bytes.size() - header_end;

        // Metadata.
        const json::Value *metadata = header.find("__metadata__");
        if (metadata == nullptr || !metadata->is_object())
        {
            return nullptr;
        }
        auto metadata_string = [&](const std::string &key) -> const std::string * {
            const json::Value *value = metadata->find(key);
            if (value == nullptr || !value->is_string())
            {
                return nullptr;
            }
            return &value->string;
        };
        const std::string *config_text = metadata_string("config");
        const std::string *vocab_text = metadata_string("vocab");
        if (config_text == nullptr || vocab_text == nullptr)
        {
            return nullptr;
        }

        auto model = std::unique_ptr<SentenceModel>(new SentenceModel());

        json::Value config_json = json::parse(*config_text);
        auto config_uint = [&](const std::string &key) -> std::size_t {
            const json::Value *value = config_json.find(key);
            if (value == nullptr || !value->is_number())
            {
                throw std::runtime_error("config missing " + key);
            }
            return static_cast<std::size_t>(value->number);
        };
        model->config_.vocab = config_uint("vocab");
        model->config_.n_layer = config_uint("n_layer");
        model->config_.n_head = config_uint("n_head");
        model->config_.n_embd = config_uint("n_embd");
        model->config_.context = config_uint("context");
        const Config &config = model->config_;
        if (config.n_head == 0 || config.n_embd == 0 || config.n_embd % config.n_head != 0 || config.vocab == 0 ||
            config.n_layer == 0 || config.context == 0)
        {
            return nullptr;
        }

        json::Value vocab_json = json::parse(*vocab_text);
        if (!vocab_json.is_array() || vocab_json.array.size() != config.vocab)
        {
            return nullptr;
        }
        for (std::size_t id = 0; id < vocab_json.array.size(); ++id)
        {
            const json::Value &item = vocab_json.array[id];
            if (!item.is_string())
            {
                continue;
            }
            std::vector<char32_t> chars = decode_utf8(item.string);
            // Reserved tokens (<pad> etc.) are addressed by constant, not by text; single code
            // points are the character vocabulary.
            if (chars.size() == 1)
            {
                model->index_[chars[0]] = static_cast<std::uint32_t>(id);
            }
        }

        if (const std::string *attribution = metadata_string("attribution"))
        {
            model->attribution_ = *attribution;
        }
        if (const std::string *license = metadata_string("license"))
        {
            model->license_ = *license;
        }

        // Collect every tensor descriptor (including the .scale companions).
        std::unordered_map<std::string, Entry> entries;
        for (const auto &field : header.object)
        {
            if (field.first == "__metadata__")
            {
                continue;
            }
            const json::Value &desc = field.second;
            if (!desc.is_object())
            {
                continue;
            }
            Entry entry;
            const json::Value *dtype = desc.find("dtype");
            const json::Value *shape = desc.find("shape");
            const json::Value *offsets = desc.find("data_offsets");
            if (dtype == nullptr || !dtype->is_string() || offsets == nullptr || !offsets->is_array() ||
                offsets->array.size() != 2)
            {
                return nullptr;
            }
            entry.dtype = dtype->string;
            if (shape != nullptr && shape->is_array())
            {
                for (const json::Value &dim : shape->array)
                {
                    entry.shape.push_back(static_cast<std::size_t>(dim.number));
                }
            }
            entry.off0 = static_cast<std::size_t>(offsets->array[0].number);
            entry.off1 = static_cast<std::size_t>(offsets->array[1].number);
            entries.emplace(field.first, std::move(entry));
        }

        auto materialize = [&](const std::string &name) -> Matrix {
            auto it = entries.find(name);
            if (it == entries.end())
            {
                throw std::runtime_error("missing tensor " + name);
            }
            const Entry &entry = it->second;
            Matrix matrix;
            std::size_t expected = product(entry.shape);
            if (entry.dtype == "F32")
            {
                matrix.floats = read_f32(data, data_len, entry);
                matrix.quantized = false;
                if (matrix.floats.size() != expected)
                {
                    throw std::runtime_error(name + ": shape does not match data");
                }
            }
            else if (entry.dtype == "F16")
            {
                matrix.floats = read_f16(data, data_len, entry);
                matrix.quantized = false;
                if (matrix.floats.size() != expected)
                {
                    throw std::runtime_error(name + ": shape does not match data");
                }
            }
            else if (entry.dtype == "I8")
            {
                auto scale_it = entries.find(name + ".scale");
                if (scale_it == entries.end())
                {
                    throw std::runtime_error(name + ": missing scale");
                }
                matrix.scales = read_f32(data, data_len, scale_it->second);
                std::size_t rows = entry.shape.empty() ? 1 : entry.shape.front();
                if (rows == 0 || matrix.scales.size() != rows)
                {
                    throw std::runtime_error(name + ": scale length does not match rows");
                }
                if (entry.off1 > data_len || entry.off0 > entry.off1)
                {
                    throw std::runtime_error(name + ": offsets out of range");
                }
                std::size_t count = entry.off1 - entry.off0;
                matrix.ints.resize(count);
                std::memcpy(matrix.ints.data(), data + entry.off0, count);
                matrix.quantized = true;
                if (matrix.ints.size() != expected)
                {
                    throw std::runtime_error(name + ": shape does not match data");
                }
            }
            else
            {
                throw std::runtime_error(name + ": unsupported dtype " + entry.dtype);
            }
            return matrix;
        };

        auto materialize_floats = [&](const std::string &name) -> std::vector<float> {
            Matrix matrix = materialize(name);
            if (matrix.quantized)
            {
                throw std::runtime_error(name + " is quantized but must be float");
            }
            return std::move(matrix.floats);
        };

        model->token_ = materialize("tok.weight");
        model->position_ = materialize_floats("pos.weight");
        model->blocks_.reserve(config.n_layer);
        for (std::size_t layer = 0; layer < config.n_layer; ++layer)
        {
            std::string prefix = "blocks." + std::to_string(layer) + ".";
            Block block;
            block.ln1_weight = materialize_floats(prefix + "ln1.weight");
            block.ln1_bias = materialize_floats(prefix + "ln1.bias");
            block.qkv_weight = materialize(prefix + "qkv.weight");
            block.qkv_bias = materialize_floats(prefix + "qkv.bias");
            block.proj_weight = materialize(prefix + "proj.weight");
            block.proj_bias = materialize_floats(prefix + "proj.bias");
            block.ln2_weight = materialize_floats(prefix + "ln2.weight");
            block.ln2_bias = materialize_floats(prefix + "ln2.bias");
            block.fc_weight = materialize(prefix + "fc.weight");
            block.fc_bias = materialize_floats(prefix + "fc.bias");
            block.out_weight = materialize(prefix + "out.weight");
            block.out_bias = materialize_floats(prefix + "out.bias");
            model->blocks_.push_back(std::move(block));
        }
        model->final_weight_ = materialize_floats("ln_f.weight");
        model->final_bias_ = materialize_floats("ln_f.bias");

        // Geometry checks: any mismatch means a malformed file, so refuse it.
        const std::size_t n_embd = config.n_embd;
        if (model->token_.len() != config.vocab * n_embd || model->position_.size() != config.context * n_embd ||
            model->final_weight_.size() != n_embd || model->final_bias_.size() != n_embd)
        {
            return nullptr;
        }
        for (const Block &block : model->blocks_)
        {
            if (block.qkv_weight.len() != 3 * n_embd * n_embd || block.proj_weight.len() != n_embd * n_embd ||
                block.fc_weight.len() != 4 * n_embd * n_embd || block.out_weight.len() != 4 * n_embd * n_embd)
            {
                return nullptr;
            }
        }
        return model;
    }
    catch (const std::exception &)
    {
        return nullptr;
    }
}

std::unique_ptr<SentenceModel> SentenceModel::load_file(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return nullptr;
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size <= 0)
    {
        return nullptr;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
    {
        return nullptr;
    }
    return load(bytes);
}

std::optional<std::uint32_t> SentenceModel::token(char32_t character) const
{
    auto it = index_.find(character);
    if (it == index_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::uint32_t> SentenceModel::encode(const std::string &text) const
{
    std::vector<char32_t> chars = decode_utf8(text);
    std::vector<std::uint32_t> out;
    out.reserve(chars.size());
    for (char32_t ch : chars)
    {
        auto it = index_.find(ch);
        out.push_back(it == index_.end() ? kUnk : it->second);
    }
    return out;
}

std::vector<float> SentenceModel::embed(const std::vector<std::uint32_t> &tokens, std::size_t width,
                                        std::size_t offset) const
{
    const std::size_t n_embd = config_.n_embd;
    std::vector<float> x(tokens.size() * n_embd, 0.0f);
    std::vector<float> token_row(n_embd, 0.0f);
    for (std::size_t step = 0; step < tokens.size(); ++step)
    {
        std::size_t token = std::min<std::size_t>(tokens[step], config_.vocab - 1);
        // Position restarts at `offset` on every batch row: each row is a separate continuation of
        // the same cached prefix, not a continuation of the row before it.
        std::size_t within = width == 0 ? step : step % width;
        std::size_t position = std::min<std::size_t>(offset + within, config_.context - 1);
        token_.row_into(token, n_embd, token_row.data());
        const float *position_row = &position_[position * n_embd];
        float *row = &x[step * n_embd];
        for (std::size_t i = 0; i < n_embd; ++i)
        {
            row[i] = token_row[i] + position_row[i];
        }
    }
    return x;
}

void SentenceModel::attention(const std::vector<float> &q, const std::vector<float> &k, const std::vector<float> &v,
                              std::size_t batch, std::size_t width, const std::vector<float> *past_k,
                              const std::vector<float> *past_v, std::size_t past_len, std::vector<float> &out,
                              std::vector<float> &scores) const
{
    const std::size_t n_embd = config_.n_embd;
    const std::size_t heads = config_.n_head;
    const std::size_t head_width = n_embd / heads;
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_width));
    if (past_k == nullptr || past_v == nullptr)
    {
        past_len = 0;
    }
    out.assign(batch * width * n_embd, 0.0f);
    scores.resize(past_len + width);
    for (std::size_t row = 0; row < batch; ++row)
    {
        const std::size_t base_row = row * width;
        for (std::size_t step = 0; step < width; ++step)
        {
            // Causal, in two runs: the whole shared prefix, then this row's own tokens up to and
            // including `step`. Rows never see each other.
            const std::size_t visible_self = step + 1;
            const std::size_t visible = past_len + visible_self;
            for (std::size_t head = 0; head < heads; ++head)
            {
                const std::size_t base = head * head_width;
                const float *query = &q[(base_row + step) * n_embd + base];
                float highest = -std::numeric_limits<float>::infinity();
                for (std::size_t j = 0; j < past_len; ++j)
                {
                    float s = dot(query, &(*past_k)[j * n_embd + base], head_width) * scale;
                    scores[j] = s;
                    highest = std::max(highest, s);
                }
                for (std::size_t j = 0; j < visible_self; ++j)
                {
                    float s = dot(query, &k[(base_row + j) * n_embd + base], head_width) * scale;
                    scores[past_len + j] = s;
                    highest = std::max(highest, s);
                }
                float sum = 0.0f;
                for (std::size_t j = 0; j < visible; ++j)
                {
                    scores[j] = std::exp(scores[j] - highest);
                    sum += scores[j];
                }
                float *target = &out[(base_row + step) * n_embd + base];
                for (std::size_t j = 0; j < past_len; ++j)
                {
                    float weight = scores[j] / sum;
                    const float *value = &(*past_v)[j * n_embd + base];
                    for (std::size_t i = 0; i < head_width; ++i)
                    {
                        target[i] += weight * value[i];
                    }
                }
                for (std::size_t j = 0; j < visible_self; ++j)
                {
                    float weight = scores[past_len + j] / sum;
                    const float *value = &v[(base_row + j) * n_embd + base];
                    for (std::size_t i = 0; i < head_width; ++i)
                    {
                        target[i] += weight * value[i];
                    }
                }
            }
        }
    }
}

void SentenceModel::feed_forward(std::vector<float> &x, const Block &block, Workspace &workspace) const
{
    const std::size_t n_embd = config_.n_embd;
    std::size_t rows = x.size() / n_embd;
    layer_norm_into(x.data(), rows, block.ln2_weight, block.ln2_bias, n_embd, workspace.normed);
    linear_into(workspace.normed, block.fc_weight, block.fc_bias, n_embd, 4 * n_embd, workspace.ff_hidden,
                workspace.linear_scratch);
    for (float &value : workspace.ff_hidden)
    {
        value = gelu(value);
    }
    linear_into(workspace.ff_hidden, block.out_weight, block.out_bias, 4 * n_embd, n_embd, workspace.ff_out,
                workspace.linear_scratch);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        x[i] += workspace.ff_out[i];
    }
}

std::vector<float> SentenceModel::forward(const std::vector<std::uint32_t> &tokens, std::size_t batch,
                                          std::size_t width, const PrefixCache *past, PrefixCache *record) const
{
    const std::size_t n_embd = config_.n_embd;
    const std::size_t rows = batch * width;
    const std::size_t past_len = past == nullptr ? 0 : past->length;
    std::vector<float> x = embed(tokens, width, past_len);
    Workspace workspace;
    if (record != nullptr)
    {
        // A cache is shared by every row that later attends to it, so it can only ever describe one
        // sequence. build_prefix is the only caller and always passes batch == 1 from position 0.
        record->keys.assign(blocks_.size(), {});
        record->values.assign(blocks_.size(), {});
        record->length = past_len + width;
    }
    for (std::size_t layer = 0; layer < blocks_.size(); ++layer)
    {
        const Block &block = blocks_[layer];
        layer_norm_into(x.data(), rows, block.ln1_weight, block.ln1_bias, n_embd, workspace.normed);
        linear_into(workspace.normed, block.qkv_weight, block.qkv_bias, n_embd, 3 * n_embd, workspace.qkv,
                    workspace.linear_scratch);
        split_qkv(workspace.qkv, rows, n_embd, workspace.q, workspace.k, workspace.v);
        const std::vector<float> *past_k = past_len == 0 ? nullptr : &past->keys[layer];
        const std::vector<float> *past_v = past_len == 0 ? nullptr : &past->values[layer];
        attention(workspace.q, workspace.k, workspace.v, batch, width, past_k, past_v, past_len, workspace.attended,
                  workspace.attention_scores);
        linear_into(workspace.attended, block.proj_weight, block.proj_bias, n_embd, n_embd, workspace.projected,
                    workspace.linear_scratch);
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            x[i] += workspace.projected[i];
        }
        feed_forward(x, block, workspace);
        if (record != nullptr)
        {
            record->keys[layer] = workspace.k;
            record->values[layer] = workspace.v;
        }
    }
    layer_norm_into(x.data(), rows, final_weight_, final_bias_, n_embd, workspace.normed);
    return std::move(workspace.normed);
}

PrefixCache SentenceModel::build_prefix(const std::vector<std::uint32_t> &tokens) const
{
    PrefixCache cache;
    if (tokens.empty())
    {
        return cache;
    }
    forward(tokens, 1, tokens.size(), nullptr, &cache);
    return cache;
}

std::vector<float> SentenceModel::distribution(const std::vector<float> &hidden) const
{
    const std::size_t n_embd = config_.n_embd;
    const std::size_t vocab = config_.vocab;
    std::vector<float> logits(vocab, 0.0f);
    float highest = -std::numeric_limits<float>::infinity();
    for (std::size_t row = 0; row < vocab; ++row)
    {
        float logit = token_.dot_row(hidden.data(), row, n_embd);
        logits[row] = logit;
        highest = std::max(highest, logit);
    }
    float sum = 0.0f;
    for (float logit : logits)
    {
        sum += std::exp(logit - highest);
    }
    float offset = highest + std::log(sum);
    for (float &logit : logits)
    {
        logit -= offset;
    }
    return logits;
}

std::vector<float> SentenceModel::target_log_probs(const std::vector<float> &hidden, std::size_t rows,
                                                   const std::vector<std::uint32_t> &targets) const
{
    const std::size_t n_embd = config_.n_embd;
    const std::size_t vocab = config_.vocab;
    std::vector<std::size_t> active_rows;
    active_rows.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row)
    {
        if (targets[row] != kNoTarget)
        {
            active_rows.push_back(row);
        }
    }
    std::vector<float> out(rows, 0.0f);
    if (active_rows.empty())
    {
        return out;
    }

    std::vector<float> logits(active_rows.size() * vocab, 0.0f);
    // Output-major over the vocabulary, and widening each row's int8 once, for the same reasons as
    // linear(): the tied embedding table is the single largest matrix in the model.
    std::vector<float> scratch(token_.quantized ? n_embd : 0, 0.0f);
    for (std::size_t id = 0; id < vocab; ++id)
    {
        const float *weights;
        float scale = 1.0f;
        if (token_.quantized)
        {
            const std::int8_t *source = &token_.ints[id * n_embd];
            for (std::size_t i = 0; i < n_embd; ++i)
            {
                scratch[i] = static_cast<float>(source[i]);
            }
            weights = scratch.data();
            scale = token_.scales[id];
        }
        else
        {
            weights = &token_.floats[id * n_embd];
        }
        for (std::size_t slot = 0; slot < active_rows.size(); ++slot)
        {
            const std::size_t row = active_rows[slot];
            logits[slot * vocab + id] = dot(&hidden[row * n_embd], weights, n_embd) * scale;
        }
    }

    for (std::size_t slot = 0; slot < active_rows.size(); ++slot)
    {
        const std::size_t row = active_rows[slot];
        const float *source = &logits[slot * vocab];
        float highest = -std::numeric_limits<float>::infinity();
        for (std::size_t id = 0; id < vocab; ++id)
        {
            highest = std::max(highest, source[id]);
        }
        float sum = 0.0f;
        for (std::size_t id = 0; id < vocab; ++id)
        {
            sum += std::exp(source[id] - highest);
        }
        out[row] = source[targets[row]] - (highest + std::log(sum));
    }
    return out;
}

std::vector<float> SentenceModel::next_log_probabilities(const std::string &text) const
{
    std::vector<std::uint32_t> tokens;
    tokens.push_back(kBos);
    std::vector<std::uint32_t> encoded = encode(text);
    std::size_t room = config_.context - 1;
    std::size_t start = encoded.size() > room ? encoded.size() - room : 0;
    tokens.insert(tokens.end(), encoded.begin() + static_cast<std::ptrdiff_t>(start), encoded.end());
    std::vector<float> hidden = forward(tokens, 1, tokens.size(), nullptr, nullptr);
    const std::size_t n_embd = config_.n_embd;
    std::vector<float> last(hidden.end() - static_cast<std::ptrdiff_t>(n_embd), hidden.end());
    return distribution(last);
}

std::vector<double> SentenceModel::score_sentences(const std::string &context,
                                                   const std::vector<std::string> &texts) const
{
    if (texts.empty())
    {
        return {};
    }
    const std::size_t limit = config_.context;
    if (limit < 2)
    {
        return std::vector<double>(texts.size(), 0.0);
    }

    // The sequence each candidate is scored under is <bos> + context + candidate. Every candidate
    // shares all of that but its own tail, so the shared part is split at its last token: that token
    // leads each batch row, because the distribution it produces is what scores the candidate's
    // first character. Only what precedes it goes in the cache, since only that stays fixed.
    std::vector<std::uint32_t> full;
    full.push_back(kBos);
    for (std::uint32_t id : encode(context))
    {
        full.push_back(id);
    }

    std::vector<std::vector<std::uint32_t>> tails;
    tails.reserve(texts.size());
    std::size_t longest = 0;
    for (const std::string &text : texts)
    {
        std::vector<std::uint32_t> ids = encode(text);
        // A candidate longer than the window minus the leading token keeps its tail, which is the
        // part the remaining context can still inform.
        if (ids.size() > limit - 1)
        {
            ids.erase(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(ids.size() - (limit - 1)));
        }
        longest = std::max(longest, ids.size());
        tails.push_back(std::move(ids));
    }
    if (longest == 0)
    {
        return std::vector<double>(texts.size(), 0.0);
    }
    // The last candidate token is a target only. Its hidden state predicts no scored character.
    const std::size_t width = longest;

    const std::uint32_t last = full.back();
    std::vector<std::uint32_t> head(full.begin(), full.end() - 1);
    // Keep the old context limit so removing the unused final token does not change scores.
    const std::size_t room = limit > longest + 1 ? limit - (longest + 1) : 0;
    if (head.size() > room)
    {
        head.erase(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(head.size() - room));
    }

    const std::size_t batch = tails.size();
    // Short rows are padded with token 0; their positions get kNoTarget and are never read.
    std::vector<std::uint32_t> flat(batch * width, 0u);
    std::vector<std::uint32_t> targets(batch * width, kNoTarget);
    for (std::size_t row = 0; row < batch; ++row)
    {
        flat[row * width] = last;
        for (std::size_t i = 0; i < tails[row].size(); ++i)
        {
            // Position i predicts token i+1, so the leading token's slot carries the first character.
            targets[row * width + i] = tails[row][i];
            if (i + 1 < tails[row].size())
            {
                flat[row * width + 1 + i] = tails[row][i];
            }
        }
    }

    std::vector<float> scored;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_tokens_ != head)
        {
            cached_prefix_ = build_prefix(head);
            cached_tokens_ = head;
        }
        std::vector<float> hidden = forward(flat, batch, width, &cached_prefix_, nullptr);
        scored = target_log_probs(hidden, batch * width, targets);
    }

    std::vector<double> out(batch, 0.0);
    for (std::size_t row = 0; row < batch; ++row)
    {
        double total = 0.0;
        for (std::size_t i = 0; i < tails[row].size(); ++i)
        {
            total += static_cast<double>(scored[row * width + i]);
        }
        out[row] = total;
    }
    return out;
}

} // namespace neural
