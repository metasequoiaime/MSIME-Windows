#include "int8_linear_avx2.h"

#include <algorithm>
#include <immintrin.h>

namespace neural
{
namespace detail
{
namespace
{

template <std::size_t BlockRows>
void linear_block(const float *x, const std::int8_t *weights, const float *scales, const float *bias, std::size_t rows,
                  std::size_t inputs, std::size_t outputs, float *out)
{
    for (std::size_t output = 0; output < outputs; ++output)
    {
        const std::int8_t *weight_row = weights + output * inputs;
        const float scale = scales[output];
        const float offset = bias == nullptr ? 0.0f : bias[output];
        for (std::size_t base = 0; base < rows; base += BlockRows)
        {
            const std::size_t count = std::min(BlockRows, rows - base);
            __m256 accumulators[BlockRows];
            for (std::size_t row = 0; row < BlockRows; ++row)
            {
                accumulators[row] = _mm256_setzero_ps();
            }
            std::size_t input = 0;
            for (; input + 8 <= inputs; input += 8)
            {
                // Each block widens the weight chunk once. Its rows share the result in a register.
                const __m128i packed = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(weight_row + input));
                const __m256 widened = _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(packed));
                for (std::size_t row = 0; row < count; ++row)
                {
                    const __m256 activation = _mm256_loadu_ps(x + (base + row) * inputs + input);
                    accumulators[row] = _mm256_fmadd_ps(activation, widened, accumulators[row]);
                }
            }
            for (std::size_t row = 0; row < count; ++row)
            {
                alignas(32) float lanes[8];
                _mm256_store_ps(lanes, accumulators[row]);
                float total = 0.0f;
                for (float lane : lanes)
                {
                    total += lane;
                }
                for (std::size_t tail = input; tail < inputs; ++tail)
                {
                    total += x[(base + row) * inputs + tail] * static_cast<float>(weight_row[tail]);
                }
                out[(base + row) * outputs + output] = total * scale + offset;
            }
        }
    }
}

} // namespace

void linear_int8_avx2(const float *x, const std::int8_t *weights, const float *scales, const float *bias,
                      std::size_t rows, std::size_t inputs, std::size_t outputs, float *out)
{
    if (rows == 1)
    {
        linear_block<1>(x, weights, scales, bias, rows, inputs, outputs, out);
    }
    else if (rows == 2)
    {
        linear_block<2>(x, weights, scales, bias, rows, inputs, outputs, out);
    }
    else
    {
        linear_block<4>(x, weights, scales, bias, rows, inputs, outputs, out);
    }
}

} // namespace detail
} // namespace neural
