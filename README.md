# Nyxen

Nyxen is an experimental C++20 audio processing engine exploring transparent signal paths, equalization, offline audio processing and continuous block-based DSP. The long-term direction is adaptive music enhancement, but adaptive algorithms and physical real-time audio processing are not implemented yet.

## Implemented components

- **NyxenCore:** JUCE-independent planar float32 processing with double-precision biquad coefficients and filter state. `Engine` provides transparent bypass and the fixed three-band `eq-reference-v1` preset.
- **ContinuousEngine:** single-owner, in-place variable-block processing, warm EQ state in bypass, mode requests, 10 ms default linear crossfade, reset/release and explicit error statuses. Concurrent calls are not supported.
- **NyxenAudio:** JUCE-based WAV/FLAC/MP3 decoding, offline block processing, audio metrics and RIFF WAV float32 output.
- **nyxen-cli:** offline bypass/EQ processing and optional `safe-gain` via Python/FFmpeg true-peak measurement and verification. The CLI refuses to overwrite existing destinations.
- **Tools/tests:** synthetic fixtures, numerical comparisons, level-matched listening preparation, output-safety checks, and an in-memory continuous stream simulator. No audio assets are bundled.

See [architecture](docs/ARCHITECTURE.md), [tests](docs/TESTING.md), [validation](docs/VALIDATION.md) and [external dependencies](docs/DEPENDENCIES.md).

## Prerequisites

Windows x64 is the validated platform. Build tools: Visual Studio C++ toolchain (2022 or newer), Windows SDK, CMake >= 3.24 and Ninja. MSVC AddressSanitizer is used by the ASan test targets. Python 3 (standard library) and FFmpeg with the required measurement filters and libmp3lame are needed for the full test suite and safe-gain utilities.

JUCE **8.0.13**, commit `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2`, is an external dependency for audio-format support. Obtain the matching [upstream revision](https://github.com/juce-framework/JUCE/tree/7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2) under `third_party/JUCE`, or pass `-DNYXEN_JUCE_DIR=<checkout>` when configuring CMake. JUCE is not bundled in this repository. Its own [AGPLv3/commercial terms](https://github.com/juce-framework/JUCE/blob/7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2/LICENSE.md) apply to the dependency; see [dependency notices](docs/DEPENDENCIES.md). CMake checks for the dependency directory but does not verify its revision.

## Build and test

From PowerShell in the project root, with JUCE in `third_party/JUCE`:

```powershell
.\scripts\build-windows.cmd
```

Or from an initialized x64 Visual Studio developer shell:

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
ctest --test-dir build -N
```

For an externally managed JUCE checkout, add `-DNYXEN_JUCE_DIR=<checkout>` to the configure command. Keep the MSVC environment available when running ASan tests. CMake detects Python/FFmpeg at configuration time. If either is missing, the number of registered tests or covered branches may be reduced; a green partial run is not equivalent to full validation.

## Usage

Input files are supplied locally. The output directory must already exist and the output WAV filename must be new.

```powershell
.\build\nyxen-cli.exe --input .\input.wav --output .\build\bypass.wav --mode bypass
.\build\nyxen-cli.exe --input .\input.wav --output .\build\eq.wav --mode eq-reference
.\build\nyxen-cli.exe --input .\input.wav --output .\build\safe.wav --mode eq-reference --output-safety safe-gain --true-peak-ceiling -1.0
.\build\nyxen-cli.exe --help
.\build\nyxen-stream-sim.exe .\build\continuous-artifacts
```

The simulator produces reports rather than audio playback. Generated logs, audio, corpus data and private listening-session materials should remain outside the repository.

## Validation

On **2026-11-10**, a Windows Release x64 build passed **10/10 CTest suites** (0 failures; 119.83 s). This includes MSVC ASan checks, four synthetic MP3 cases and 63 numerical precision cases. This is a recorded result, not a claim about every machine or physical playback. See the [validation record](docs/VALIDATION.md).

## Current limits and next steps

M0.3 is a continuous **in-memory simulation**, not a deployed real-time audio system. There is no device callback, WASAPI/APO/VST3 integration, player/UI, adaptive bass/clarity algorithm or completed perceptual listening evaluation. The reference EQ can exceed full scale; `safe-gain` is offline only. Core processing does not resample, limit or normalize. RIFF output has an approximately 4 GiB limit; RF64/BW64 and more than two channels are unsupported.

Next work involves reproducing the build in a clean environment, controlled listening evaluation, host ownership/command protocol and device-level callback, latency and underrun testing. Simulation timings alone do not guarantee real-time deadlines or audible improvement.
