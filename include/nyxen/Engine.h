#pragma once
#include <nyxen/Biquad.h>
#include <cstddef>
#include <span>

namespace nyxen {
enum class ProcessingMode { Bypass, EqReference };
struct ProcessSpec {
    double sampleRate;
    std::size_t channels;
    std::size_t maximumBlockSize;
};

// Planar float samples. No files, codecs, CLI or device dependencies.
class Engine {
public:
    void prepare(ProcessSpec spec, ProcessingMode mode = ProcessingMode::Bypass,
                 const EqPreset& preset = referencePreset);
    void reset() noexcept;
    void process(std::span<float* const> channels, std::size_t frames);
    const ProcessSpec& spec() const noexcept { return spec_; }
private:
    ProcessSpec spec_{};
    bool prepared_ = false;
    ProcessingMode mode_ = ProcessingMode::Bypass;
    std::array<Biquad, 3> filters_;
};
}
