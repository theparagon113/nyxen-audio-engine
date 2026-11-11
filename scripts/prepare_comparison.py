"""Prepare one immutable bypass/EQ comparison; no absolute loudness target."""
import argparse
import json
from pathlib import Path
import shutil
import sys

from audio_experiment import command, constant_gain, json_new, measure, sha256


def prepare(source, output, cli, ffmpeg, ceiling=-1.0):
    source, output, cli = Path(source).resolve(), Path(output).resolve(), Path(cli).resolve()
    if not source.is_file() or not cli.is_file() or not (-20 <= ceiling <= -0.1):
        raise ValueError('Invalid source, CLI or true-peak ceiling (-20..-0.1 dBTP)')
    output.mkdir(parents=True, exist_ok=False)  # every run needs a new directory
    raw = output / 'raw'
    raw.mkdir()
    report = dict(schema_version=1, status='UNFIT', source=str(source), source_sha256=sha256(source),
                  cli_sha256=sha256(cli), ffmpeg_version=command([ffmpeg, '-version']).stdout.splitlines()[0],
                  preset_id='eq-reference-v1', true_peak_ceiling_dbtp=ceiling,
                  loudness_tolerance_lu=0.2, measured_acceptance_lu=0.15,
                  method='FFmpeg ebur128: gated integrated metadata, oversampled true peak summary; dualmono=false',
                  raw={}, attempts=[])
    raw_paths = {}
    try:
        for mode in ('bypass', 'eq-reference'):
            path = raw / f'{mode}.wav'
            result = command([cli, '--input', source, '--output', path, '--mode', mode])
            if mode == 'eq-reference' and 'Preset: eq-reference-v1' not in result.stdout:
                raise ValueError('Unexpected reference preset')
            with open(raw / f'{mode}-cli.txt', 'x', encoding='utf-8') as s:
                s.write(result.stdout)
            raw_paths[mode] = path
            report['raw'][mode] = dict(path=str(path), sha256=sha256(path),
                                      **measure(path, ffmpeg, raw / f'{mode}-measurement.txt'))
        dimensions = ('frames', 'sample_rate', 'channels')
        a, b = report['raw']['bypass'], report['raw']['eq-reference']
        if any(a[k] != b[k] for k in dimensions):
            raise ValueError('Audio dimensions do not match')
        if not all(x['integrated_valid'] for x in (a, b)):
            raise ValueError('Integrated loudness invalid (silence, too short or below gate); no listening pair')
        target = min(a['integrated_lufs'], b['integrated_lufs'])
        gains = {mode: target - measurement['integrated_lufs'] for mode, measurement in report['raw'].items()}
        for attempt in range(1, 5):
            excess = max(report['raw'][m]['true_peak_upper_dbtp'] + gains[m] - ceiling for m in gains)
            common_attenuation = max(0, excess + 0.05)  # additional rounding/float guard
            gains = {m: g - common_attenuation for m, g in gains.items()}
            folder = output / f'matched-{attempt}'
            folder.mkdir()
            measurements = {}
            for mode, path in raw_paths.items():
                target_path = folder / f'{mode}.wav'
                constant_gain(path, target_path, gains[mode])
                measurements[mode] = dict(path=str(target_path), sha256=sha256(target_path), gain_db=gains[mode],
                                          **measure(target_path, ffmpeg, folder / f'{mode}-measurement.txt'))
            ma, mb = measurements['bypass'], measurements['eq-reference']
            valid = all(x['integrated_valid'] for x in (ma, mb))
            delta = abs(ma['integrated_lufs'] - mb['integrated_lufs']) if valid else None
            report['attempts'].append(dict(index=attempt, common_attenuation_db=common_attenuation,
                                           loudness_difference_lu=delta, versions=measurements))
            if any(any(x[k] != a[k] for k in dimensions) for x in (ma, mb)):
                raise ValueError('Matched copy dimensions changed')
            if not valid:
                raise ValueError('Loudness became invalid after attenuation; pair not suitable')
            safe = all(x['sample_peak'] < 1 and x['true_peak_upper_dbtp'] <= ceiling for x in (ma, mb))
            if delta <= 0.15 and safe:
                report.update(status='PASS', matched=measurements, loudness_difference_lu=delta,
                              target_relative_lufs=target, inserted_delay_frames=0,
                              minimum_true_peak_margin_db=min(-x['true_peak_upper_dbtp'] for x in (ma, mb)))
                break
            # Re-gating can change the level relation. Re-render from raw with
            # corrected constant gains, then measure again. No dynamic processing.
            lower = min(ma['integrated_lufs'], mb['integrated_lufs'])
            for mode, measurement in measurements.items():
                gains[mode] += lower - measurement['integrated_lufs']
                if measurement['true_peak_upper_dbtp'] > ceiling:
                    reduction = measurement['true_peak_upper_dbtp'] - ceiling + 0.05
                    gains = {m: g - reduction for m, g in gains.items()}
        if report['status'] != 'PASS':
            report['reason'] = 'Cannot meet measured loudness and peak constraints after four attempts'
        if sha256(source) != report['source_sha256']:
            raise ValueError('Source changed during preparation')
    except Exception as e:
        report['status'] = 'UNFIT'
        report.pop('matched', None)
        report['reason'] = str(e)
    json_new(output / 'comparison.json', report)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', required=True, type=Path)
    p.add_argument('--output-dir', required=True, type=Path)
    p.add_argument('--cli', type=Path, default=Path(__file__).resolve().parents[1] / 'build' / 'nyxen-cli.exe')
    p.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or 'ffmpeg')
    p.add_argument('--true-peak-ceiling', type=float, default=-1.0)
    args = p.parse_args()
    try:
        r = prepare(args.input, args.output_dir, args.cli, args.ffmpeg, args.true_peak_ceiling)
        print(json.dumps({k: r[k] for k in ('status', 'reason', 'loudness_difference_lu', 'minimum_true_peak_margin_db') if k in r}, indent=2))
        return 0 if r['status'] == 'PASS' else 2
    except Exception as e:
        print(f'Error: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
