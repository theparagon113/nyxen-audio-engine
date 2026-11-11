#include <nyxen/Engine.h>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace nyxen {
void Engine::prepare(ProcessSpec spec, ProcessingMode mode, const EqPreset& preset) {
    if (!std::isfinite(spec.sampleRate) || spec.sampleRate <= 0
        || (spec.channels != 1 && spec.channels != 2) || spec.maximumBlockSize == 0)
        throw std::invalid_argument("Invalid processing specification");
    if (mode != ProcessingMode::Bypass && mode != ProcessingMode::EqReference)
        throw std::invalid_argument("Invalid processing mode");
    auto filters = filters_;
    if (mode == ProcessingMode::EqReference)
        for (std::size_t i = 0; i < filters.size(); ++i) filters[i].prepare(spec.sampleRate, preset[i]);
    filters_ = filters;
    spec_ = spec;
    mode_ = mode;
    prepared_ = true;
    reset();
}
void Engine::reset() noexcept { for (auto& filter : filters_) filter.reset(); }
void Engine::process(std::span<float* const> channels, std::size_t frames) {
    if (!prepared_ || channels.size() != spec_.channels || frames > spec_.maximumBlockSize)
        throw std::invalid_argument("Invalid audio block or engine not prepared");
    for (auto* channel : channels)
        if (channel == nullptr && frames != 0)
            throw std::invalid_argument("Null audio channel");
    // Preserve the M0.1 path: no sample arithmetic or buffer copies in bypass.
    if (mode_ == ProcessingMode::Bypass) return;
    for (std::size_t ch = 0; ch < channels.size(); ++ch)
        for (std::size_t n = 0; n < frames; ++n) {
            double sample = channels[ch][n];
            for (auto& filter : filters_) sample = filter.processSample(sample, ch);
            if (std::abs(sample) > std::numeric_limits<float>::max())
                throw std::runtime_error("EQ exceeds float32 range");
            channels[ch][n] = static_cast<float>(sample);
        }
}
}
