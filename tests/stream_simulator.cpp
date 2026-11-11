#include <nyxen/ContinuousEngine.h>
#include "rt_allocation_probe.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

using namespace nyxen;
using Signal = std::vector<std::vector<float>>;
constexpr double pi = std::numbers::pi;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Fn> void rejects(Fn fn) { bool rejected = false; try { fn(); } catch (const std::exception&) { rejected = true; } check(rejected, "invalid configuration accepted"); }
StreamStatus callback(ContinuousEngine& e, std::span<float* const> channels, std::size_t frames) {
    const auto before = rtprobe::events();
    StreamStatus status;
    { rtprobe::Region region; status = e.processBlock(channels, frames); }
    check(rtprobe::events() == before, "observed allocation inside processBlock");
    return status;
}
Signal fixture(int rate, int channels, const std::string& kind, int frames = 8197) {
    Signal result(channels, std::vector<float>(frames));
    std::uint32_t seed = 0xdecafbad;
    for (int ch = 0; ch < channels; ++ch) for (int n = 0; n < frames; ++n) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const double noise = (double(seed) / 4294967295.0 - 0.5) * 0.8;
        const double t = double(n) / rate;
        double x = 0;
        if (kind == "impulse") x = (n == 0 || n == 1024 || n == frames - 1) ? (ch ? -0.8 : 0.8) : 0;
        if (kind == "sine") x = 0.7 * std::sin(2 * pi * (997 + ch * 503) * t);
        if (kind == "noise") x = noise;
        if (kind == "multitone") x = 0.2 * (std::sin(2 * pi * 80 * t) + std::sin(2 * pi * 350 * t) + std::sin(2 * pi * 6500 * t));
        if (kind == "near-full-scale" || kind == "headroom") x = (kind == "headroom" ? 1.25 : 0.99) * std::sin(2 * pi * 40 * t + ch * 0.1);
        result[ch][n] = float(x);
    }
    return result;
}
Signal reference(const Signal& input, int rate) {
    auto output = input;
    Engine e;
    e.prepare({double(rate), output.size(), output[0].size()}, ProcessingMode::EqReference);
    std::array<float*, 2> p{};
    for (std::size_t ch = 0; ch < output.size(); ++ch) p[ch] = output[ch].data();
    e.process({p.data(), output.size()}, output[0].size());
    return output;
}
Signal partitioned(const Signal& input, int rate, ProcessingMode mode, const std::vector<int>& pattern) {
    constexpr float sentinel = -12345.25f;
    Signal guarded(input.size(), std::vector<float>(input[0].size() + 2, sentinel));
    for (std::size_t ch = 0; ch < input.size(); ++ch) std::copy(input[ch].begin(), input[ch].end(), guarded[ch].begin() + 1);
    ContinuousEngine e;
    e.prepare({double(rate), input.size(), 1024}, mode);
    std::array<float*, 2> p{};
    std::size_t part = 0;
    for (std::size_t offset = 0; offset < input[0].size();) {
        const auto frames = std::min<std::size_t>(pattern[part++ % pattern.size()], input[0].size() - offset);
        for (std::size_t ch = 0; ch < input.size(); ++ch) p[ch] = guarded[ch].data() + 1 + offset;
        check(callback(e, {p.data(), input.size()}, frames) == StreamStatus::Ok, "partition status");
        offset += frames;
    }
    Signal output;
    for (const auto& channel : guarded) {
        check(channel.front() == sentinel && channel.back() == sentinel, "guard corruption");
        output.emplace_back(channel.begin() + 1, channel.end() - 1);
    }
    return output;
}
double difference(const Signal& a, const Signal& b) {
    double maximum = 0;
    for (std::size_t ch = 0; ch < a.size(); ++ch) for (std::size_t n = 0; n < a[ch].size(); ++n) {
        check(std::isfinite(b[ch][n]), "nonfinite output");
        maximum = std::max(maximum, std::abs(double(a[ch][n]) - b[ch][n]));
    }
    return maximum;
}
struct Event { std::size_t frame; ProcessingMode mode; };
struct FadeMetrics { double error = 0, innovation = 0, hardJump = 0, maximumPathDifference = 0; };
FadeMetrics fadeTest(const Signal& input, int rate, const std::vector<int>& pattern, Signal* captured = nullptr) {
    const std::vector<Event> events{{73, ProcessingMode::EqReference}, {73, ProcessingMode::Bypass}, {73, ProcessingMode::EqReference},
        {337, ProcessingMode::Bypass}, {380, ProcessingMode::EqReference}, {401, ProcessingMode::EqReference},
        {1023, ProcessingMode::Bypass}, {1100, ProcessingMode::Bypass}, {2048, ProcessingMode::EqReference},
        {4096, ProcessingMode::Bypass}, {6000, ProcessingMode::EqReference}, {7000, ProcessingMode::Bypass}};
    std::vector<std::vector<double>> wet(input.size(), std::vector<double>(input[0].size()));
    std::array<Biquad, 3> filters;
    for (std::size_t i = 0; i < filters.size(); ++i) filters[i].prepare(rate, referencePreset[i]);
    for (std::size_t ch = 0; ch < input.size(); ++ch) for (std::size_t n = 0; n < input[ch].size(); ++n) {
        double x = input[ch][n]; for (auto& f : filters) x = f.processSample(x, ch); wet[ch][n] = x;
    }
    auto output = input;
    ContinuousEngine e;
    e.prepare({double(rate), input.size(), 1024});
    std::size_t index = 0, part = 0;
    std::array<float*, 2> p{};
    for (std::size_t offset = 0; offset < input[0].size();) {
        while (index < events.size() && events[index].frame == offset) check(e.requestMode(events[index++].mode) == StreamStatus::Ok, "request mode");
        const auto nextEvent = index < events.size() ? events[index].frame : input[0].size();
        const auto frames = std::min<std::size_t>({std::size_t(pattern[part++ % pattern.size()]), input[0].size() - offset, nextEvent - offset});
        check(frames > 0, "event partition");
        for (std::size_t ch = 0; ch < input.size(); ++ch) p[ch] = output[ch].data() + offset;
        check(callback(e, {p.data(), input.size()}, frames) == StreamStatus::Ok, "fade callback");
        offset += frames;
    }
    // Independent sample schedule and unquantized wet reference, no telemetry used.
    FadeMetrics metrics;
    double mix = 0, step = 0;
    std::size_t remaining = 0, event = 0;
    auto target = ProcessingMode::Bypass;
    const auto fadeFrames = std::size_t(std::llround(rate * 0.01));
    for (std::size_t n = 0; n < input[0].size(); ++n) {
        auto requested = target;
        while (event < events.size() && events[event].frame == n) requested = events[event++].mode;
        const bool changed = requested != target;
        if (changed) { target = requested; remaining = fadeFrames; step = ((target == ProcessingMode::EqReference ? 1.0 : 0.0) - mix) / fadeFrames; }
        const double previous = mix;
        if (remaining) { mix = remaining == 1 ? (target == ProcessingMode::EqReference ? 1 : 0) : std::clamp(mix + step, 0.0, 1.0); --remaining; }
        for (std::size_t ch = 0; ch < input.size(); ++ch) {
            const double dry = input[ch][n], processed = wet[ch][n];
            const double expected = (1 - mix) * dry + mix * processed;
            const double frozen = (1 - previous) * dry + previous * processed;
            metrics.error = std::max(metrics.error, std::abs(double(output[ch][n]) - float(expected)));
            metrics.innovation = std::max(metrics.innovation, std::abs(double(output[ch][n]) - frozen));
            metrics.maximumPathDifference = std::max(metrics.maximumPathDifference, std::abs(processed - dry));
            if (changed) metrics.hardJump = std::max(metrics.hardJump, std::abs((target == ProcessingMode::EqReference ? 1.0 : 0.0) - previous) * std::abs(processed - dry));
            check(std::isfinite(output[ch][n]), "fade nonfinite");
            check(std::abs(double(output[ch][n])) <= std::max(std::abs(dry), std::abs(processed)) + 2e-7, "crossfade gain sum overflow");
        }
    }
    check(metrics.error <= 1e-7, "fade reference mismatch");
    check(metrics.innovation <= metrics.maximumPathDifference / fadeFrames + 2e-7, "fade slew bound");
    check(metrics.innovation <= 0.003, "mode-induced innovation exceeds 0.003 FS for fixtures/default 10 ms");
    if (captured) *captured = std::move(output);
    return metrics;
}
std::size_t privateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX c{}; c.cb = sizeof(c);
    check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c)) != 0, "memory stats");
    return c.PrivateUsage;
#else
    return 0;
#endif
}
void contracts() {
    static_assert(noexcept(std::declval<ContinuousEngine&>().processBlock({}, 0)));
    static_assert(noexcept(std::declval<ContinuousEngine&>().requestMode(ProcessingMode::Bypass)));
    ContinuousEngine e;
    check(callback(e, {}, 0) == StreamStatus::NotPrepared, "unprepared");
    check(e.requestMode(ProcessingMode::EqReference) == StreamStatus::NotPrepared, "request before prepare");
    for (const double rate : {0.0, 22050.0, double(NAN), double(INFINITY)}) rejects([&] { e.prepare({rate, 2, 1024}); });
    for (const std::size_t channels : {0, 3}) rejects([&] { e.prepare({48000, channels, 1024}); });
    rejects([&] { e.prepare({48000, 2, 0}); });
    rejects([&] { e.prepare({48000, 2, 1024}, ProcessingMode::Bypass, 0); });
    rejects([&] { e.prepare({48000, 2, 1024}, ProcessingMode::Bypass, NAN); });
    rejects([&] { e.prepare({48000, 2, 1024}, static_cast<ProcessingMode>(99)); });
    e.prepare({48000, 2, 1024});
    std::array<float, 1025> left{}, right{};
    std::array<float*, 2> p{left.data(), right.data()}, null{nullptr, nullptr};
    check(callback(e, p, 1025) == StreamStatus::InvalidBlock, "capacity");
    check(callback(e, {p.data(), 1}, 1) == StreamStatus::InvalidBlock, "channel count");
    check(callback(e, null, 1) == StreamStatus::InvalidBlock, "null");
    check(callback(e, null, 0) == StreamStatus::Ok, "zero frames");
    check(e.requestMode(static_cast<ProcessingMode>(99)) == StreamStatus::InvalidMode, "mode enum");
    check(e.requestMode(ProcessingMode::EqReference) == StreamStatus::Ok, "pending request");
    check(callback(e, null, 0) == StreamStatus::Ok && e.currentEqMix() == 0, "zero frames advanced request");
    left[17] = NAN;
    check(callback(e, p, 64) == StreamStatus::InvalidSamples && e.currentEqMix() == 0, "NaN validation mutated state");
    left[17] = INFINITY;
    check(callback(e, p, 64) == StreamStatus::InvalidSamples, "infinity"); left[17] = 0;
    auto alias = p; alias[1] = left.data();
    check(callback(e, alias, 64) == StreamStatus::InvalidBlock, "alias channels");
    alias[1] = left.data() + 1;
    check(callback(e, alias, 64) == StreamStatus::InvalidBlock, "overlap channels");
    alias[1] = reinterpret_cast<float*>(reinterpret_cast<char*>(right.data()) + 1);
    check(callback(e, alias, 64) == StreamStatus::InvalidBlock, "misaligned pointer");
    check(callback(e, p, 64) == StreamStatus::Ok && e.remainingTransitionFrames() == 480 - 64, "fade length");
    const auto before = e.remainingTransitionFrames();
    e.requestMode(ProcessingMode::EqReference);
    check(callback(e, p, 64) == StreamStatus::Ok && e.remainingTransitionFrames() == before - 64, "duplicate request restarted fade");
    e.reset(); check(e.currentEqMix() == 0 && e.remainingTransitionFrames() == 0, "reset initial mode");
    e.release(); e.release(); check(callback(e, p, 64) == StreamStatus::NotPrepared, "release");
    e.prepare({48000, 2, 1024}, ProcessingMode::EqReference);
    left.fill(std::numeric_limits<float>::max());
    check(callback(e, p, 64) == StreamStatus::NumericOverflow, "float overflow should fail, not clip");
    check(std::all_of(left.begin(), left.end(), [](float x) { return std::isfinite(x); }), "overflow wrote nonfinite");
    check(callback(e, p, 64) == StreamStatus::Faulted && e.requestMode(ProcessingMode::Bypass) == StreamStatus::Faulted, "fault latch");
    e.reset(); left.fill(0); check(callback(e, p, 64) == StreamStatus::Ok, "fault reset");
    e.prepare({48000, 1, 1024}, ProcessingMode::Bypass, 20);
    check(e.transitionFrames() == 960, "configurable fade duration");
    // Reset and preparation reproduce exactly a new stream with the same input.
    auto x = fixture(48000, 2, "noise", 512), original = x;
    e.prepare({48000, 2, 1024}, ProcessingMode::EqReference);
    p = {x[0].data(), x[1].data()}; check(callback(e, p, 512) == StreamStatus::Ok, "first stream");
    const auto first = x;
    e.reset(); x = original; p = {x[0].data(), x[1].data()}; check(callback(e, p, 512) == StreamStatus::Ok && x == first, "new stream reset");
}
double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    const double index = p * (values.size() - 1); const auto low = std::size_t(index);
    return values[low] + (values[std::min(low + 1, values.size() - 1)] - values[low]) * (index - low);
}
void benchmark(std::ofstream& out) {
    out << "sample_rate,channels,frames,mode,iterations,mean_us,median_us,p95_us,p99_us,max_us,budget_us,p95_percent,p99_percent,max_percent\n" << std::setprecision(12);
    for (const int rate : {44100, 48000, 96000}) for (const int channels : {1, 2}) for (const int block : {64, 128, 256, 512, 1024})
        for (const std::string mode : {"bypass", "eq", "transition"}) {
            const auto original = fixture(rate, channels, "noise", block);
            auto data = original;
            std::array<float*, 2> p{}; for (int ch = 0; ch < channels; ++ch) p[ch] = data[ch].data();
            ContinuousEngine e;
            // Long configurable fade keeps even 1024-frame measurements in transition.
            // Same per-frame arithmetic as the default 10 ms; re-target each iteration.
            e.prepare({double(rate), std::size_t(channels), 1024}, mode == "eq" ? ProcessingMode::EqReference : ProcessingMode::Bypass, mode == "transition" ? 1000 : 10);
            std::vector<double> times(1000);
            for (int iteration = -100; iteration < 1000; ++iteration) {
                for (int ch = 0; ch < channels; ++ch) std::copy(original[ch].begin(), original[ch].end(), data[ch].begin());
                if (mode == "transition") e.requestMode(iteration % 2 ? ProcessingMode::EqReference : ProcessingMode::Bypass);
                StreamStatus status;
                const auto before = rtprobe::events();
                const auto start = std::chrono::steady_clock::now();
                { rtprobe::Region region; status = e.processBlock({p.data(), std::size_t(channels)}, block); }
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                check(status == StreamStatus::Ok && rtprobe::events() == before, "benchmark callback failed/allocated");
                if (iteration >= 0) times[iteration] = us;
            }
            double sum = 0; for (auto t : times) sum += t;
            const double budget = 1e6 * block / rate, p95 = percentile(times, 0.95), p99 = percentile(times, 0.99), maximum = *std::max_element(times.begin(), times.end());
            out << rate << ',' << channels << ',' << block << ',' << mode << ",1000," << sum / times.size() << ',' << percentile(times, 0.5) << ',' << p95 << ',' << p99 << ',' << maximum << ','
                << budget << ',' << p95 / budget * 100 << ',' << p99 / budget * 100 << ',' << maximum / budget * 100 << '\n';
        }
}
int main(int argc, char** argv) {
    try {
#ifdef NYXEN_STREAM_ASAN
        if (argc == 2 && std::string(argv[1]) == "--asan-positive-control") {
            auto* allocation = new float[1];
            volatile std::size_t index = 2;
            allocation[index] = 1.0f; // deliberate fault, ONLY in the sanitizer self-test child
            std::cerr << allocation[index];
            delete[] allocation;
            return 0;
        }
#endif
        check(argc == 2 && sizeof(void*) == 8, "simulator requires artifact directory and x64");
#ifndef NDEBUG
        throw std::runtime_error("simulator benchmarking requires Release");
#endif
#ifndef NYXEN_STREAM_ASAN
        const auto probeBefore = rtprobe::events();
        {
            rtprobe::Region region;
            auto* a = ::operator new(32); auto* b = ::operator new[](33); auto* c = ::operator new(34, std::align_val_t(64));
            auto* d = ::operator new[](35, std::align_val_t(64)); auto* e = ::operator new(36, std::nothrow);
            auto* f = ::operator new(37, std::align_val_t(64), std::nothrow);
            ::operator delete(a); ::operator delete[](b); ::operator delete(c, std::align_val_t(64));
            ::operator delete[](d, std::align_val_t(64)); ::operator delete(e); ::operator delete(f, std::align_val_t(64));
        }
        check(rtprobe::events() - probeBefore == 6, "allocation detector self-test");
#endif
        const auto callbackStart = rtprobe::events();
        const auto folder = std::filesystem::absolute(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(folder);
        std::ofstream result(folder / "validation.txt"), fade(folder / "crossfade.csv"), stats(folder / "benchmark.csv"), metadata(folder / "environment.txt");
        fade << "sample_rate,channels,signal,max_reference_error,max_mode_innovation,abrupt_reference_jump,max_path_difference\n" << std::setprecision(12);
        metadata << "Release x64, C++20; engine bytes=" << sizeof(ContinuousEngine) << "\n";
#ifdef _MSC_VER
        metadata << "MSVC _MSC_VER=" << _MSC_VER << " _MSC_FULL_VER=" << _MSC_FULL_VER << "\n";
#endif
#ifdef _WIN32
        char cpu[256]{}; DWORD bytes = sizeof(cpu);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &bytes) == ERROR_SUCCESS)
            metadata << "CPU=" << cpu << "\n";
#endif
        contracts();
        double maximumStaticError = 0, maximumFadeError = 0, maximumInnovation = 0, maximumHardJump = 0, maximumLongError = 0;
        int staticCases = 0, fadeCases = 0;
        const std::vector<std::vector<int>> patterns{{64}, {128}, {256}, {512}, {1024}, {37}, {1}, {1, 37, 64, 128, 256, 512, 1024, 7}};
        for (const int rate : {44100, 48000, 96000}) for (const int channels : {1, 2})
            for (const std::string kind : {"silence", "impulse", "sine", "noise", "multitone", "near-full-scale", "headroom"}) {
                const auto input = fixture(rate, channels, kind);
                const auto offline = reference(input, rate);
                for (const auto& pattern : patterns) {
                    const auto eq = partitioned(input, rate, ProcessingMode::EqReference, pattern);
                    const auto d = difference(offline, eq); maximumStaticError = std::max(maximumStaticError, d);
                    check(d <= 1e-7, "continuous vs unpartitioned reference");
                    const auto bypass = partitioned(input, rate, ProcessingMode::Bypass, pattern);
                    for (int ch = 0; ch < channels; ++ch) check(std::memcmp(input[ch].data(), bypass[ch].data(), input[ch].size() * sizeof(float)) == 0, "exact bypass representation");
                    ++staticCases;
                }
                Signal a, b;
                const auto m = fadeTest(input, rate, {1024}, &a);
                fadeTest(input, rate, patterns.back(), &b);
                check(difference(a, b) <= 1e-7, "fade variable partitions differ");
                maximumFadeError = std::max(maximumFadeError, m.error);
                maximumInnovation = std::max(maximumInnovation, m.innovation);
                maximumHardJump = std::max(maximumHardJump, m.hardJump);
                fade << rate << ',' << channels << ',' << kind << ',' << m.error << ',' << m.innovation << ',' << m.hardJump << ',' << m.maximumPathDifference << '\n';
                ++fadeCases;
            }
        // Streaming through 180 seconds at each Fs, stereo; never retains the whole signal.
        std::uint64_t totalFrames = 0;
        std::ptrdiff_t largestPrivateGrowth = 0;
        for (const int rate : {44100, 48000, 96000}) {
            Signal block(2, std::vector<float>(1024)), expected = block, dry = block;
            ContinuousEngine stream; stream.prepare({double(rate), 2, 1024});
            Engine offline; offline.prepare({double(rate), 2, 1024}, ProcessingMode::EqReference);
            const std::vector<int> sizes{1, 37, 64, 128, 256, 512, 1024, 7};
            std::uint32_t seed = 0x13572468; double maxObserved = 0;
            auto feed = [&](std::uint64_t count, bool warmup) {
                std::size_t part = 0;
                double mix = 0, step = 0; std::size_t remaining = 0; auto target = ProcessingMode::Bypass;
                for (std::uint64_t offset = 0; offset < count;) {
                    const auto n = std::min<std::uint64_t>(sizes[part % sizes.size()], count - offset);
                    if (part % 17 == 0) {
                        target = target == ProcessingMode::Bypass ? ProcessingMode::EqReference : ProcessingMode::Bypass;
                        stream.requestMode(target); remaining = stream.transitionFrames();
                        step = ((target == ProcessingMode::EqReference ? 1.0 : 0.0) - mix) / remaining;
                    }
                    for (std::size_t i = 0; i < n; ++i) {
                        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                        block[0][i] = float((double(seed) / 4294967295.0 - 0.5) * 2.5); block[1][i] = 0;
                        expected[0][i] = dry[0][i] = block[0][i]; expected[1][i] = dry[1][i] = 0;
                    }
                    std::array<float*, 2> p{block[0].data(), block[1].data()}, q{expected[0].data(), expected[1].data()};
                    offline.process(q, std::size_t(n)); // reference outside instrumented callback
                    check(callback(stream, p, std::size_t(n)) == StreamStatus::Ok, "long stream status");
                    for (std::size_t i = 0; i < n; ++i) {
                        if (remaining) { mix = remaining == 1 ? (target == ProcessingMode::EqReference ? 1 : 0) : std::clamp(mix + step, 0.0, 1.0); --remaining; }
                        check(std::isfinite(block[0][i]) && block[1][i] == 0, "long stream finite/crosstalk");
                        const auto known = float((1 - mix) * dry[0][i] + mix * expected[0][i]);
                        maximumLongError = std::max(maximumLongError, std::abs(double(block[0][i]) - known));
                        check(std::abs(double(block[0][i]) - known) <= 2e-7, "long-stream continuous history/reference");
                        maxObserved = std::max(maxObserved, std::abs(double(block[0][i])));
                    }
                    offset += n; ++part;
                }
                if (!warmup) totalFrames += count;
            };
            feed(rate, true);
            stream.reset(); offline.reset();
            const auto liveBefore = rtprobe::liveBytes(), privateBefore = privateBytes();
            feed(std::uint64_t(rate) * 180, false);
            const auto privateAfter = privateBytes(), liveAfter = rtprobe::liveBytes();
#ifndef NYXEN_STREAM_ASAN
            check(liveAfter == liveBefore, "C++ heap grew during long streaming");
#endif
            largestPrivateGrowth = std::max(largestPrivateGrowth, std::ptrdiff_t(privateAfter) - std::ptrdiff_t(privateBefore));
            check(maxObserved > 1, "headroom was implicitly clipped");
        }
#ifndef NYXEN_STREAM_ASAN
        benchmark(stats);
        const auto allocations = rtprobe::events() - callbackStart;
        check(allocations == 0, "observed callback allocation count");
        result << "C++ new/new[] observed in callback regions: " << allocations << "\n";
#else
        result << "ASan run: allocation replacement disabled; benchmark not run\n";
#endif
        result << std::setprecision(12) << "PASS continuous: " << staticCases << " static partition cases, " << fadeCases << " fade schedules\n"
            << "Max static reference error: " << maximumStaticError << "\nMax fade reference error: " << maximumFadeError
            << "\nMax mode-induced innovation: " << maximumInnovation << " (threshold 0.003 FS)\nAbrupt switch reference max: " << maximumHardJump
            << "\nMax long-stream reference error (wet reference quantized float32): " << maximumLongError
            << "\nLong stream total frames: " << totalFrames << " (180 s stereo at each Fs; 540 s total)\n"
            << "Largest process private-byte growth observed: " << largestPrivateGrowth << "\n";
        result.flush(); check(result && fade && stats && metadata, "artifact writes");
        std::ifstream read(folder / "validation.txt"); std::cout << read.rdbuf();
        std::cout << "Artifacts: " << folder.string() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL continuous simulator: " << e.what() << '\n'; return 1; }
}
