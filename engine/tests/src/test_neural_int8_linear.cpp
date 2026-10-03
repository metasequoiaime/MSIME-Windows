#include "neural/int8_linear_avx2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main()
{
    if (!neural::detail::supports_avx2_fma())
    {
        std::puts("Skipped: AVX2 and FMA are not available.");
        return 0;
    }

    for (std::size_t rows : {1u, 2u, 3u, 4u, 5u, 8u, 9u, 16u})
    {
        for (std::size_t inputs : {1u, 7u, 8u, 9u, 15u, 16u, 17u, 192u, 448u})
        {
            for (std::size_t outputs : {1u, 3u, 11u})
            {
                std::vector<float> x(rows * inputs);
                std::vector<std::int8_t> weights(inputs * outputs);
                std::vector<float> scales(outputs);
                std::vector<float> bias(outputs);
                std::vector<float> actual(rows * outputs);
                for (std::size_t i = 0; i < x.size(); ++i)
                {
                    x[i] = std::sin(static_cast<float>(i) * 0.071f);
                }
                for (std::size_t i = 0; i < weights.size(); ++i)
                {
                    weights[i] = static_cast<std::int8_t>(static_cast<int>(i * 17u % 255u) - 127);
                }
                for (std::size_t i = 0; i < outputs; ++i)
                {
                    scales[i] = 0.002f + static_cast<float>(i % 9) * 0.001f;
                    bias[i] = std::sin(static_cast<float>(i) * 0.03f);
                }

                for (bool with_bias : {false, true})
                {
                    neural::detail::linear_int8_avx2(x.data(), weights.data(), scales.data(),
                                                     with_bias ? bias.data() : nullptr, rows, inputs, outputs,
                                                     actual.data());
                    for (std::size_t row = 0; row < rows; ++row)
                    {
                        for (std::size_t output = 0; output < outputs; ++output)
                        {
                            float total = 0.0f;
                            for (std::size_t input = 0; input < inputs; ++input)
                            {
                                total += x[row * inputs + input] * static_cast<float>(weights[output * inputs + input]);
                            }
                            const float expected = total * scales[output] + (with_bias ? bias[output] : 0.0f);
                            if (!(std::fabs(actual[row * outputs + output] - expected) <= 1e-4f))
                            {
                                std::fprintf(stderr, "int8 linear mismatch: rows=%zu inputs=%zu outputs=%zu\n", rows,
                                             inputs, outputs);
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}
