#pragma once
#include <nyxen/Engine.h>

namespace nyxen {
enum class StreamStatus {
    Ok, NotPrepared, InvalidBlock, InvalidSamples, InvalidMode, NumericOverflow, Faulted
};

// Single-owner API. Never call prepare/reset/release/requestMode/processBlock
// concurrently. There is intentionally no shared-thread mailbox in M0.3.
class ContinuousEngine {
public:
    // Control path only; throws on invalid configuration. No heap resources.
    void prepare(ProcessSpec spec, ProcessingMode initialMode = ProcessingMode::Bypass,
                 double transitionMilliseconds = 10.0, const EqPreset& preset = referencePreset);
    void reset() noexcept; // new stream: initial mode, zero filter histories
    void release() noexcept;
    StreamStatus requestMode(ProcessingMode mode) noexcept; // last request wins
    StreamStatus processBlock(std::span<float* const> channels, std::size_t frames) noexcept;

    const ProcessSpec& spec() const noexcept { return spec_; }
    double currentEqMix() const noexcept { return mix_; }
    std::size_t transitionFrames() const noexcept { return fadeFrames_; }
    std::size_t remainingTransitionFrames() const noexcept { return remaining_; }
private:
    ProcessSpec spec_{};
    std::array<Biquad, 3> filters_{};
    ProcessingMode initial_ = ProcessingMode::Bypass;
    ProcessingMode requested_ = ProcessingMode::Bypass;
    ProcessingMode target_ = ProcessingMode::Bypass;
    double mix_ = 0, step_ = 0;
    std::size_t fadeFrames_ = 0, remaining_ = 0;
    bool prepared_ = false, faulted_ = false;
};
}
