#include <nyxen/ContinuousEngine.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace nyxen {
void ContinuousEngine::prepare(ProcessSpec spec, ProcessingMode initialMode,
                               double milliseconds, const EqPreset& preset) {
    if ((spec.sampleRate != 44100 && spec.sampleRate != 48000 && spec.sampleRate != 96000)
        || (spec.channels != 1 && spec.channels != 2) || spec.maximumBlockSize == 0
        || spec.maximumBlockSize > 1048576 || !std::isfinite(milliseconds)
        || milliseconds <= 0 || milliseconds > 1000
        || (initialMode != ProcessingMode::Bypass && initialMode != ProcessingMode::EqReference))
        throw std::invalid_argument("Invalid continuous stream configuration");
    std::array<Biquad, 3> filters;
    for (std::size_t i = 0; i < filters.size(); ++i) filters[i].prepare(spec.sampleRate, preset[i]);
    // Commit only after all validation/preparation succeeded.
    filters_ = filters;
    spec_ = spec;
    initial_ = initialMode;
    fadeFrames_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(spec.sampleRate * milliseconds / 1000)));
    prepared_ = true;
    reset();
}
void ContinuousEngine::reset() noexcept {
    for (auto& filter : filters_) filter.reset();
    requested_ = target_ = initial_;
    mix_ = initial_ == ProcessingMode::EqReference ? 1.0 : 0.0;
    step_ = 0;
    remaining_ = 0;
    faulted_ = false;
}
void ContinuousEngine::release() noexcept {
    reset();
    prepared_ = false;
    spec_ = {};
    fadeFrames_ = 0;
}
StreamStatus ContinuousEngine::requestMode(ProcessingMode mode) noexcept {
    if (!prepared_) return StreamStatus::NotPrepared;
    if (mode != ProcessingMode::Bypass && mode != ProcessingMode::EqReference) return StreamStatus::InvalidMode;
    if (faulted_) return StreamStatus::Faulted;
    requested_ = mode;
    return StreamStatus::Ok;
}
StreamStatus ContinuousEngine::processBlock(std::span<float* const> channels, std::size_t frames) noexcept {
    if (!prepared_) return StreamStatus::NotPrepared;
    if (faulted_) return StreamStatus::Faulted;
    if (channels.size() != spec_.channels || frames > spec_.maximumBlockSize) return StreamStatus::InvalidBlock;
    if (frames == 0) return StreamStatus::Ok; // no dereference, request consumption or state advance
    // Check layout BEFORE accessing samples. Caller still owns actual buffer capacity.
    const auto bytes = frames * sizeof(float);
    std::array<std::uintptr_t, 2> starts{};
    for (std::size_t ch = 0; ch < channels.size(); ++ch) {
        starts[ch] = reinterpret_cast<std::uintptr_t>(channels[ch]);
        if (!channels[ch] || starts[ch] % alignof(float) != 0
            || starts[ch] > std::numeric_limits<std::uintptr_t>::max() - bytes) return StreamStatus::InvalidBlock;
    }
    if (channels.size() == 2 && starts[0] < starts[1] + bytes && starts[1] < starts[0] + bytes)
        return StreamStatus::InvalidBlock; // overlapping planar channels are forbidden
    for (std::size_t ch = 0; ch < channels.size(); ++ch)
        for (std::size_t n = 0; n < frames; ++n)
            if (!std::isfinite(channels[ch][n])) return StreamStatus::InvalidSamples;

    if (requested_ != target_) {
        target_ = requested_;
        const double end = target_ == ProcessingMode::EqReference ? 1.0 : 0.0;
        remaining_ = mix_ == end ? 0 : fadeFrames_;
        step_ = remaining_ ? (end - mix_) / static_cast<double>(remaining_) : 0;
    }
    for (std::size_t n = 0; n < frames; ++n) {
        const double end = target_ == ProcessingMode::EqReference ? 1.0 : 0.0;
        const double nextMix = remaining_ == 1 ? end : std::clamp(mix_ + (remaining_ ? step_ : 0), 0.0, 1.0);
        std::array<float, 2> output{};
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            const double dry = channels[ch][n];
            double wet = dry;
            // Keep the EQ warm in bypass: transitions never start with stale history.
            for (auto& filter : filters_)
                if (!filter.tryProcessSample(wet, ch, wet)) {
                    faulted_ = true;
                    return StreamStatus::NumericOverflow;
                }
            const double value = nextMix == 0 ? dry : nextMix == 1 ? wet : (1 - nextMix) * dry + nextMix * wet;
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                faulted_ = true;
                return StreamStatus::NumericOverflow;
            }
            output[ch] = static_cast<float>(value);
        }
        if (nextMix != 0) for (std::size_t ch = 0; ch < channels.size(); ++ch) channels[ch][n] = output[ch];
        // Bypass deliberately leaves the original float representation untouched.
        mix_ = nextMix;
        if (remaining_) --remaining_;
    }
    return StreamStatus::Ok;
}
}
