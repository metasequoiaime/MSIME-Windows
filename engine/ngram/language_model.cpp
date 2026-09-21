#include "language_model.h"

#include <cmath>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <new>
#include <type_traits>

#include "lm/config.hh"
#include "lm/model.hh"
#include "lm/state.hh"
#include "lm/word_index.hh"
#include "util/string_piece.hh"

namespace ngram
{
namespace
{

// 与 libime 的 DEFAULT_LANGUAGE_MODEL_UNKNOWN_PROBABILITY_PENALTY 取同一个值
// （libime/core/constants.h）。log10(1/6e7) ≈ -7.78。出货的 sc.lm 就是按这个
// 量级标定的，换数值等于整体重调未登录词相对已登录词的代价。
constexpr float kUnknownProbability = 1.0F / 60000000.0F;

// -a 22 -q 4 trie 生成的就是这个模型类型，必须与 tools/build_binary_main.cc
// 的调用参数保持一致，否则载入时会抛 FormatLoadException。
using Model = lm::ngram::QuantArrayTrieModel;

static_assert(sizeof(lm::ngram::State) <= kStateSize, "State storage too small; check KENLM_MAX_ORDER");
static_assert(alignof(lm::ngram::State) <= 8, "State storage under-aligned");
static_assert(std::is_standard_layout_v<lm::ngram::State> && std::is_trivial_v<lm::ngram::State>,
              "lm::ngram::State must stay POD for the raw-storage cast below");
static_assert(std::is_same_v<WordIndex, lm::WordIndex>, "WordIndex must match kenlm's");

// kenlm 只认窄字符路径。path::string() 在 Windows 上按 ANSI 代码页转换，遇到代码
// 页外的字符会抛 std::system_error（"No mapping for the Unicode character..."），
// 所以这里一律走 UTF-8，由 kenlm_file_io.h 在打开时转回宽字符。
std::string to_utf8(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

lm::ngram::State &as_lm(State &state)
{
    return *reinterpret_cast<lm::ngram::State *>(state.data);
}

const lm::ngram::State &as_lm(const State &state)
{
    return *reinterpret_cast<const lm::ngram::State *>(state.data);
}

} // namespace

class LanguageModel::Impl
{
  public:
    std::unique_ptr<Model> model;
    std::string error;
    State begin_state;
    State null_state;
    float unknown_penalty = std::log10(kUnknownProbability);
};

LanguageModel::LanguageModel(const std::filesystem::path &model_file) : impl_(std::make_unique<Impl>())
{
    if (model_file.empty())
    {
        impl_->error = "empty model path";
        return;
    }
    if (!std::filesystem::exists(model_file))
    {
        // kenlm 打不开文件时抛 ErrnoException，而它的错误消息经 StringStream::AdvanceTo
        // 解引用 std::string 的 end 迭代器（kenlm 上游代码，MSVC Debug STL 下是断言 +
        // fastfail，任何 catch 都接不住）。缺失的模型先在这里拦下，让「文件不存在」走
        // 正常的 error 通道；exists 对目录也返回 true，那种边缘错误仍留给 kenlm。
        impl_->error = "language model file not found";
        return;
    }
    try
    {
        lm::ngram::Config config;
        // 出货的模型不带 <s> / </s>，与 libime 一致地静默接受，否则 kenlm 会
        // 在载入时抛异常。
        config.sentence_marker_missing = lm::SILENT;
        impl_->model = std::make_unique<Model>(to_utf8(model_file).c_str(), config);
        as_lm(impl_->begin_state) = impl_->model->BeginSentenceState();
        as_lm(impl_->null_state) = impl_->model->NullContextState();
    }
    catch (const std::exception &e)
    {
        impl_->model.reset();
        impl_->error = e.what();
    }
    catch (...)
    {
        impl_->model.reset();
        impl_->error = "unknown error while loading language model";
    }
}

LanguageModel::~LanguageModel() = default;

bool LanguageModel::valid() const
{
    return impl_->model != nullptr;
}

const std::string &LanguageModel::error() const
{
    return impl_->error;
}

WordIndex LanguageModel::index(std::string_view word) const
{
    if (!impl_->model)
        return 0;
    return impl_->model->GetVocabulary().Index(StringPiece{word.data(), word.size()});
}

WordIndex LanguageModel::unknown() const
{
    if (!impl_->model)
        return 0;
    return impl_->model->GetVocabulary().NotFound();
}

bool LanguageModel::is_unknown(WordIndex idx) const
{
    return idx == unknown();
}

const State &LanguageModel::begin_state() const
{
    return impl_->begin_state;
}

const State &LanguageModel::null_state() const
{
    return impl_->null_state;
}

float LanguageModel::score(const State &in, WordIndex idx, State &out) const
{
    if (!impl_->model)
        return impl_->unknown_penalty;
    // kenlm 的 Score 要求 in 与 out 不是同一个对象。
    if (&in == &out)
    {
        State scratch = in;
        return score(scratch, idx, out);
    }
    const float s = impl_->model->Score(as_lm(in), idx, as_lm(out));
    return s + (is_unknown(idx) ? impl_->unknown_penalty : 0.0F);
}

float LanguageModel::score(const State &in, std::string_view word, State &out) const
{
    return score(in, index(word), out);
}

float LanguageModel::single_word_score(const State &in, std::string_view word) const
{
    State discarded;
    return score(in, word, discarded);
}

void LanguageModel::set_unknown_penalty(float penalty)
{
    impl_->unknown_penalty = penalty;
}

float LanguageModel::unknown_penalty() const
{
    return impl_->unknown_penalty;
}

const LanguageModel &shared_language_model(const std::filesystem::path &model_file)
{
    // 故意泄漏：模型可能在静态析构期还被其他单例引用，销毁顺序无法保证，而进程
    // 退出时操作系统会回收这块 mmap。
    static std::mutex mutex;
    static auto *cache = new std::map<std::string, std::unique_ptr<LanguageModel>>();
    const std::lock_guard<std::mutex> guard(mutex);
    auto &slot = (*cache)[to_utf8(model_file)];
    if (!slot)
        slot = std::make_unique<LanguageModel>(model_file);
    return *slot;
}

} // namespace ngram
