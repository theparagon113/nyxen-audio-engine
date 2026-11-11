# Implemented architecture

```text
nyxen-cli -> NyxenAudio / OfflineProcessor -> JUCE readers and WAV writer
                                         -> NyxenCore / Engine -> Biquad cascade
nyxen-stream-sim -> NyxenCore / ContinuousEngine -> Biquad cascade
Python tools -> CLI subprocesses + external FFmpeg measurements
```

`NyxenCore` has no file/device/JUCE dependency. It accepts borrowed planar float32 buffers and keeps double-precision coefficients and filter state. The reference preset is a low shelf at 100 Hz, +1.5 dB, S=1; a peak at 350 Hz, -1 dB, Q=0.8; and a high shelf at 4500 Hz, +1 dB, S=1. Coefficient formulas follow the [Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/); see dependency provenance.

`Engine` exposes prepare/reset/process for offline use. `ContinuousEngine` adds prepare, `processBlock noexcept`, `requestMode`, reset, and release. One owner must serialize all calls including getters. It supports 44.1/48/96 kHz, one or two channels, a prepared maximum of 1..1048576 frames, and finite fade times in (0,1000] ms. Default fade is 10 ms. Filters stay warm in bypass; stable bypass does not write the samples. Changing modes linearly interpolates dry/wet over the configured number of frames, retaining state across partial/variable blocks and reversals. There is no extra block queue or lookahead; this does not describe physical device latency.

The caller owns valid, aligned, nonoverlapping channel buffers with enough storage. Empty blocks do not advance state. Invalid layout or nonfinite input is rejected before processing. Numeric overflow can follow already processed frames; the failing frame is not partially committed across channels and the stream enters a faulted state. The host must discard a failed block and coordinate reset or prepare. See `include/nyxen/ContinuousEngine.h` and its existing tests for exact contracts. Values above full scale are valid and are not implicitly clipped.

`OfflineProcessor` decodes using JUCE, uses 4096-frame blocks by default, records peak/RMS and dimensions, and writes IEEE float32 RIFF WAV. It rejects existing outputs and uses temporary output followed by non-replacing publication. The source is not rewritten. Only WAV/FLAC/MP3 readers are registered; tags are not carried through. RMS is not LUFS.

The CLI's `safe-gain` launches the source-tree Python script. It renders raw DSP, measures via FFmpeg, applies constant attenuation if needed, and remeasures the float32 output against the ceiling. It fails if measurement or convergence is invalid. Preserve mode is the default. Compiled binaries embed configure-time Python/script paths; moving a built executable alone is unsupported. Reconfigure/rebuild after moving the source or interpreter.

Python scripts use only the standard library. `prepare_comparison.py` creates level-matched copies; `generate_session.py` creates anonymized participant material and a private organizer answer key; `inspect_corpus.py` validates locally supplied fragments. The 15-entry corpus template remains pending. Outputs may include private source paths, hashes, metadata, and responses and must not be published. `limiter_reference.py` is an external FFmpeg experiment, not an implemented engine limiter. `run_precision_benchmark.py` compares numerical paths with a high-precision Decimal reference; it does not change production precision.
