#pragma once
#include <array>
#include <cstddef>

namespace nyxen {
enum class FilterType { LowShelf, Peaking, HighShelf };
struct EqBand {
    FilterType type;
    double frequencyHz;
    double gainDb;
    double shape; // Q for peaking; S (shelf slope) for shelves
};
struct BiquadCoefficients { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
using EqPreset = std::array<EqBand, 3>;
inline constexpr const char* referencePresetId = "eq-reference-v1";
inline constexpr EqPreset referencePreset{{
    {FilterType::LowShelf, 100.0, 1.5, 1.0},
    {FilterType::Peaking, 350.0, -1.0, 0.8},
    {FilterType::HighShelf, 4500.0, 1.0, 1.0}
}};

// RBJ Audio EQ Cookbook; normalized a0=1, transposed direct form II.
// Double coefficients/state; two independent channel histories.
class Biquad {
public:
    void prepare(double sampleRate, EqBand band);
    void reset() noexcept;
    double processSample(double input, std::size_t channel);
    bool tryProcessSample(double input, std::size_t channel, double& output) noexcept;
    const BiquadCoefficients& coefficients() const noexcept { return coefficients_; }
private:
    struct State { double z1 = 0, z2 = 0; };
    BiquadCoefficients coefficients_;
    std::array<State, 2> states_{};
};
}
