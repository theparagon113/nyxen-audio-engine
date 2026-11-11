"""Optional external experiment only: FFmpeg oversampled alimiter, not Nyxen DSP.
Not a certified true-peak limiter; accept only if the actual result passes the meter.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import statistics
import sys
import time

from audio_experiment import command, float_wav_info, json_new, measure, samples, sha256


def transient_metrics(source, target, channels, rate):
    x = [v for b in samples(source) for v in b]
    y = [v for b in samples(target) for v in b]
    if len(x) != len(y):
        raise ValueError('Reference changed frames')
    xx = math.fsum(v * v for v in x)
    fitted = math.fsum(a * b for a, b in zip(x, y)) / xx if xx else 0
    residual = math.sqrt(math.fsum((b - fitted * a) ** 2 for a, b in zip(x, y)) / len(x))
    width = (rate // 50) * channels
    local_gains = []
    for start in range(0, len(x) - width + 1, width):
        a = math.fsum(v * v for v in x[start:start + width])
        b = math.fsum(v * v for v in y[start:start + width])
        if a > 1e-15 and b > 0:
            local_gains.append(10 * math.log10(b / a))
    return dict(fitted_constant_gain=fitted, residual_to_constant_gain_rms=residual,
                local_rms_gain_20ms_min_db=min(local_gains) if local_gains else None,
                local_rms_gain_20ms_max_db=max(local_gains) if local_gains else None)


def evaluate(raw, safe, output, ffmpeg, ceiling=-1):
    raw, safe, output = Path(raw).resolve(), Path(safe).resolve(), Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    info = float_wav_info(raw)
    hashes = [sha256(raw), sha256(safe)]
    before = measure(raw, ffmpeg, output / 'raw-measurement.txt')
    safe_measurement = measure(safe, ffmpeg, output / 'safe-measurement.txt')
    # Guard for reconstruction after downsampling: fixed -0.3 dB, not auto LUFS.
    limit = 10 ** ((ceiling - 0.3) / 20)
    filtergraph = (f'aresample=192000,alimiter=limit={limit:.17g}:attack=5:release=50:level=0:latency=1,'
                   f"aresample={info['sample_rate']},atrim=end_sample={info['frames']}")
    elapsed = []
    commands = []
    for n in range(3):
        target = output / f'limiter-{n}.wav'
        args = [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-n', '-i', raw,
                '-af', filtergraph, '-c:a', 'pcm_f32le', target]
        start = time.perf_counter()
        command(args)
        elapsed.append(time.perf_counter() - start)
        commands.append([str(x) for x in args])
    after = measure(output / 'limiter-0.wav', ffmpeg, output / 'limiter-measurement.txt')
    same = all(before[k] == after[k] == safe_measurement[k] for k in ('frames', 'sample_rate', 'channels'))
    met = (after['true_peak_valid'] and not after['true_peak_silence'] and after['true_peak_upper_dbtp'] <= ceiling)
    report = dict(status='VERIFIED_REFERENCE' if met and same else 'PENDING_UNSUITABLE',
                  scope='External oversampled alimiter reference; not a certified limiter and not a production option',
                  ffmpeg_version=command([ffmpeg, '-version']).stdout.splitlines()[0],
                  filtergraph=filtergraph, limiter_lookahead_ms=5, latency_compensation=True,
                  wall_time_median_seconds=statistics.median(elapsed), wall_time_range_seconds=max(elapsed) - min(elapsed),
                  timing_samples_seconds=elapsed, timing_includes='FFmpeg startup, processing, resampling, file I/O; excludes measurement',
                  commands=commands, before=before, safe_gain=safe_measurement, limiter=after,
                  safe_gain_transient_metrics=transient_metrics(raw, safe, info['channels'], info['sample_rate']),
                  limiter_transient_metrics=transient_metrics(raw, output / 'limiter-0.wav', info['channels'], info['sample_rate']))
    if before['integrated_valid'] and after['integrated_valid']:
        report['limiter_loudness_change_lu'] = after['integrated_lufs'] - before['integrated_lufs']
        report['safe_gain_loudness_change_lu'] = safe_measurement['integrated_lufs'] - before['integrated_lufs']
    report['limiter_rms_level_change_db'] = 20 * math.log10(after['rms'] / before['rms']) if after['rms'] and before['rms'] else None
    if [sha256(raw), sha256(safe)] != hashes:
        raise ValueError('Inputs changed')
    json_new(output / 'limiter-reference.json', report)
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--raw', type=Path, required=True)
    p.add_argument('--safe', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or 'ffmpeg')
    args = p.parse_args()
    try:
        r = evaluate(args.raw, args.safe, args.output_dir, args.ffmpeg)
        print(json.dumps({k: r[k] for k in ('status', 'limiter_loudness_change_lu', 'safe_gain_loudness_change_lu', 'wall_time_median_seconds') if k in r}, indent=2))
    except Exception as e:
        print(f'Optional reference PENDING: {e}', file=sys.stderr)
        sys.exit(1)
