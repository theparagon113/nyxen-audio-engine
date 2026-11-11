"""Binary32/64 comparison against an 80-digit Decimal arithmetic reference."""
from array import array
import argparse
import csv
from decimal import Decimal, localcontext
import json
import math
from pathlib import Path
import sys
import uuid

from audio_experiment import command, float_wav_info, json_new, samples, sha256


def read_binary(path, code):
    data = array(code)
    data.frombytes(path.read_bytes())
    if sys.byteorder != 'little':
        data.byteswap()
    return data


def reference(input_values, coefficients, repetitions, frames, precision=80):
    with localcontext() as context:
        context.prec = precision
        result = []
        for channel in (0, 1):
            state = [[Decimal(0), Decimal(0)] for _ in range(3 * repetitions)]
            for i in range(frames):
                x = Decimal.from_float(input_values[channel * frames + i])
                for section, c in enumerate(coefficients * repetitions):
                    b0, b1, b2, a1, a2 = c
                    z1, z2 = state[section]
                    y = b0 * x + z1
                    state[section] = [b1 * x - a1 * y + z2, b2 * x - a2 * y]
                    x = y
                result.append(+x)
        return result


def error(values, ref):
    with localcontext() as context:
        context.prec = 80
        differences = [Decimal.from_float(x) - y for x, y in zip(values, ref)]
        maximum = max(abs(x) for x in differences)
        rms = (sum(x * x for x in differences) / len(ref)).sqrt()
        ref_rms = (sum(x * x for x in ref) / len(ref)).sqrt()
        actual_rms = (sum(Decimal.from_float(x) ** 2 for x in values) / len(ref)).sqrt()
        return dict(max_abs=float(maximum), rms_error=float(rms), reference_rms=float(ref_rms),
                    error_dbfs=20 * math.log10(float(rms)) if rms else None,
                    snr_db=20 * math.log10(float(ref_rms / rms)) if ref_rms and rms else None,
                    rms_level_difference_db=20 * math.log10(float(actual_rms / ref_rms)) if actual_rms and ref_rms else None)


def run(executable, base):
    folder = Path(base).resolve() / str(uuid.uuid4())
    result = command([executable, folder])
    print(result.stdout.strip())
    with (folder / 'coefficients.csv').open() as s:
        coefficients = {}
        for row in csv.DictReader(s):
            coefficients.setdefault(int(row['sample_rate']), []).append(
                [Decimal.from_float(float.fromhex(row[k])) for k in ('b0', 'b1', 'b2', 'a1', 'a2')])
    reports = []
    convergence = 0.0
    with (folder / 'cases.csv').open() as s:
        cases = list(csv.DictReader(s))
    for case in cases:
        rate, repetitions, frames = (int(case[k]) for k in ('sample_rate', 'preset_repetitions', 'frames'))
        stem = case['stem']
        original = read_binary(folder / f'{stem}-input.f32', 'f')
        a = read_binary(folder / f'{stem}-a.f32', 'f')
        b = read_binary(folder / f'{stem}-b.f64', 'd')
        if len(original) != frames * 2 or len(a) != len(original) or len(b) != len(original):
            raise AssertionError('benchmark dimensions')
        ref = reference(original, coefficients[rate], repetitions, frames)
        if case['signal'] in ('noise', 'low-amplitude') and repetitions == 12:
            ref100 = reference(original, coefficients[rate], repetitions, frames, 100)
            with localcontext() as context:
                context.prec = 100
                difference = max(abs(x - y) for x, y in zip(ref, ref100))
            convergence = max(convergence, float(difference))
            if difference > Decimal('1e-60'):
                raise AssertionError('Decimal reference did not converge')
        exported = []
        for name, expected in (('a', a), ('b', b)):
            path = folder / f'{stem}-{name}.wav'
            info = float_wav_info(path)
            if info['sample_rate'] != rate or info['frames'] != frames or info['channels'] != 2:
                raise AssertionError('float32 exported dimensions')
            actual = [x for block in samples(path) for x in block]  # interleaved WAV
            canonical = array('f', (expected[ch * frames + n] for n in range(frames) for ch in (0, 1)))
            if actual != list(canonical):
                raise AssertionError('export changed float32 samples')
            exported.append(actual)
        post = error(exported[0], [Decimal.from_float(x) for x in exported[1]])
        metrics_a, metrics_b = error(a, ref), error(b, ref)
        if case['signal'] == 'silence' and (metrics_a['max_abs'] != 0 or metrics_b['max_abs'] != 0):
            raise AssertionError('silence precision')
        if repetitions == 1 and post['max_abs'] != 0:
            raise AssertionError('single production preset differs after float32 export')
        reports.append(dict(**case, a=metrics_a, b=metrics_b, float32_export_difference=post))
    with (folder / 'cpu.csv').open() as s:
        cpu = list(csv.DictReader(s))
    if len(reports) != 63 or len(cpu) != 9 or any(int(c['iterations']) != 11 or
            float(c['a_median_ms']) <= 0 or float(c['b_median_ms']) <= 0 for c in cpu):
        raise AssertionError('CPU/numerical benchmark was not fully executed')
    report = dict(status='PASS', reference='80-digit Decimal DF-II-T, exact binary64 coefficients and identical float32 decoded-like inputs',
                  convergence_80_vs_100_max_abs=convergence, cases=reports, cpu=cpu,
                  executable_sha256=sha256(executable), python_version=sys.version,
                  cpu_method='Release x64 DSP-only steady_clock wall-time, median/IQR, 11 alternating A/B iterations + 2 warmups; no I/O/preparation/copies',
                  artifacts=str(folder))
    json_new(folder / 'precision-report.json', report)
    print(f'PASS precision: {len(reports)} Decimal-reference cases; 80/100 digit convergence {convergence:.3g}')
    for repetitions in (1, 4, 12):
        group = [r for r in reports if int(r['preset_repetitions']) == repetitions]
        print(f"Preset repetitions {repetitions}: max A={max(x['a']['max_abs'] for x in group):.9g}, "
              f"max B={max(x['b']['max_abs'] for x in group):.9g}, "
              f"post-export A/B max={max(x['float32_export_difference']['max_abs'] for x in group):.9g}")
    print(f'Artifacts: {folder}')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    args = p.parse_args()
    try:
        run(args.executable.resolve(), args.output_dir)
    except Exception as e:
        print(f'FAIL precision benchmark: {e}', file=sys.stderr)
        sys.exit(1)
