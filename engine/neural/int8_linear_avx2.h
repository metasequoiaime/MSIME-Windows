#pragma once

#include <cstddef>
#include <cstdint>

namespace neural
{
namespace detail
{

bool supports_avx2_fma();

void linear_int8_avx2(const float *x, const std::int8_t *weights, const float *scales, const float *bias,
                      std::size_t rows, std::size_t inputs, std::size_t outputs, float *out);

} // namespace detail
} // namespace neural
