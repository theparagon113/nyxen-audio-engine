# External dependencies and attribution

Nyxen currently requires external dependencies for audio I/O and tooling. No third-party source checkout, executable, compiled binary or audio asset is bundled here.

| Dependency/reference | Version/source | Role and licensing notes |
| --- | --- | --- |
| JUCE | [Upstream](https://github.com/juce-framework/JUCE), 8.0.13, commit `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2` | Audio format decoding/writing and transitive modules. See the [revision's notices](https://github.com/juce-framework/JUCE/blob/7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2/LICENSE.md) and [JUCE 8 EULA](https://juce.com/legal/juce-8-licence/). JUCE is licensed under AGPLv3 or an applicable commercial EULA. |
| FLAC within JUCE | Pinned JUCE `modules/juce_audio_formats/codecs/flac` | FLAC codec; upstream BSD-style notices apply. |
| zlib within JUCE | Pinned JUCE `modules/juce_core/zip/zlib` | Embedded zlib implementation; upstream zlib notices apply. |
| MP3 reader within JUCE | Pinned JUCE `juce_MP3AudioFormat.cpp` | MP3 decoding is enabled by `JUCE_USE_MP3AUDIOFORMAT=1`; the upstream implementation includes a third-party-IP disclaimer. |
| FFmpeg / libmp3lame | [FFmpeg](https://ffmpeg.org/) | External executable for analysis and synthetic MP3 generation. Build-dependent [FFmpeg licensing](https://ffmpeg.org/legal.html). No FFmpeg executable or libraries are redistributed. |
| Python 3 | [Python](https://www.python.org/) | Standard-library scripts and Decimal comparison. |
| CMake, Ninja, MSVC, Windows SDK | Tool vendors | Build and test tools, supplied separately. |
| Audio EQ Cookbook | [W3C Audio Working Group Note](https://www.w3.org/TR/2021/NOTE-audio-eq-cookbook-20210608/), Robert Bristow-Johnson | Mathematical basis of the biquad coefficients; see attribution below. |

JUCE's requested module chain is `juce_audio_formats -> juce_audio_basics -> juce_core`. Direct JUCE use is confined to `src/OfflineProcessor.cpp`, `src/main.cpp`, `tests/tests.cpp` and `tests/precision_benchmark.cpp`. `NyxenCore` and the stream simulator do not directly depend on JUCE. The top-level CMake configuration currently still requires it.

The JUCE MP3 reader's upstream notice does not guarantee the decoder is free of third-party intellectual-property restrictions. No MP3 implementation or resulting binaries are distributed here. FLAC and zlib implementations remain part of externally acquired JUCE, with their upstream notices. A separately installed GPL-enabled FFmpeg build was used during validation; FFmpeg is invoked as an external process rather than vendored.

Third-party notices and applicable dependency conditions remain relevant if the project is built or distributed, independently of whether Nyxen ships their source.

## Audio EQ Cookbook attribution

The coefficient formulas in `src/Biquad.cpp` follow Robert Bristow-Johnson's Audio EQ Cookbook as published in the W3C Audio Working Group Note of 8 June 2021. Refer to the [original note](https://www.w3.org/TR/2021/NOTE-audio-eq-cookbook-20210608/) and the [W3C document license](https://www.w3.org/copyright/document-license-2015/).

W3C derivative-material notice, retained for material derived from the reference:
> Copyright © 2015 W3C® (MIT, ERCIM, Keio, Beihang). This software or document includes material copied from or derived from Audio EQ Cookbook, https://www.w3.org/TR/2021/NOTE-audio-eq-cookbook-20210608/.

The referenced publication is © 2021 W3C (MIT, ERCIM, Keio, Beihang). The mathematical attribution does not establish ownership of the original document or other third-party material.
