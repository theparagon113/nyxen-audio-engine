"""Optional whole-file output protection. Nyxen DSP remains unchanged."""
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile

from audio_experiment import command, constant_gain, measure, sha256

MAX_CORRECTIONS = 2  # initial render plus at most two conservative corrections
GUARD_DB = 0.05


def ceiling_met(measurement, ceiling):
    return measurement['true_peak_valid'] and (measurement['true_peak_silence'] or
        measurement['true_peak_upper_dbtp'] <= ceiling)


def require_tp(measurement):
    if not measurement['true_peak_valid']:
        raise RuntimeError('True peak measurement is unavailable/invalid; safe-gain cannot certify output')


def publish_no_replace(source, destination):
    if os.name == 'nt':
        move = ctypes.WinDLL('kernel32', use_last_error=True).MoveFileExW
        move.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
        move.restype = ctypes.c_int
        if not move(str(source), str(destination), 0):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        os.link(source, destination)  # atomic no replacement, same-volume temporary
        os.unlink(source)


def protect(source, output, mode, cli, ffmpeg, ceiling=-1.0):
    source, output, cli = Path(source).resolve(), Path(output).absolute(), Path(cli).resolve()
    if not math.isfinite(ceiling) or not -20 <= ceiling <= -0.1:
        raise ValueError('true peak ceiling must be -20..-0.1 dBTP')
    if os.path.lexists(output):
        raise FileExistsError('Output already exists; overwriting is forbidden')
    if output.suffix.lower() != '.wav' or not output.parent.is_dir():
        raise ValueError('Output must be a new .wav in an existing directory')
    if not source.is_file() or not cli.is_file() or mode not in ('bypass', 'eq-reference'):
        raise ValueError('Invalid input, CLI or mode')
    source_hash = sha256(source)
    version = command([ffmpeg, '-version']).stdout.splitlines()[0]
    # The unique sibling directory is deleted on success AND any exception.
    with tempfile.TemporaryDirectory(prefix='.nyxen-safety-', dir=output.parent) as work:
        work = Path(work)
        raw = work / 'processed.wav'
        dsp = command([cli, '--input', source, '--output', raw, '--mode', mode,
                       '--output-safety', 'preserve'])
        before = measure(raw, ffmpeg, work / 'before.txt')
        require_tp(before)
        raw_hash = sha256(raw)
        gain = 0.0
        if not ceiling_met(before, ceiling):
            gain = min(0.0, ceiling - before['true_peak_upper_dbtp'] - GUARD_DB)
        attempts = []
        for index in range(MAX_CORRECTIONS + 1):
            candidate = work / f'candidate-{index}.wav'
            if gain == 0:
                # Preserve all float samples and the writer's file representation.
                with raw.open('rb') as s, candidate.open('xb') as d:
                    shutil.copyfileobj(s, d)
            else:
                constant_gain(raw, candidate, gain)  # always from raw, quantize once
            after = measure(candidate, ffmpeg, work / f'after-{index}.txt')
            require_tp(after)
            if any(before[k] != after[k] for k in ('frames', 'sample_rate', 'channels')):
                raise RuntimeError('Output safety changed audio dimensions')
            attempts.append(dict(gain_db=gain, true_peak_dbtp=after['true_peak_dbtp'],
                                 true_peak_upper_dbtp=after['true_peak_upper_dbtp']))
            if ceiling_met(after, ceiling):
                break
            if index == MAX_CORRECTIONS:
                raise RuntimeError('True peak ceiling not met after two corrections; output not published')
            gain -= max(GUARD_DB, after['true_peak_upper_dbtp'] - ceiling + GUARD_DB)
        if sha256(source) != source_hash or sha256(raw) != raw_hash:
            raise RuntimeError('Original or raw DSP result changed')
        report = dict(output_safety='safe-gain', true_peak_ceiling_dbtp=ceiling,
                      verification='PASS', applied_gain_db=gain, attenuation_required=gain < 0,
                      corrections=len(attempts) - 1, attempts=attempts, ffmpeg_version=version,
                      source_sha256=source_hash, raw_sha256=raw_hash, output_sha256=sha256(candidate),
                      method='FFmpeg ebur128 true peak summary, +0.05 dB rounding bound; no LUFS target',
                      before=before, after=after)
        publish_no_replace(candidate, output)
    return dsp.stdout, report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', choices=('bypass', 'eq-reference'), required=True)
    p.add_argument('--cli', type=Path, required=True)
    p.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or 'ffmpeg')
    p.add_argument('--true-peak-ceiling', type=float, default=-1.0)
    args = p.parse_args()
    try:
        dsp, r = protect(args.input, args.output, args.mode, args.cli, args.ffmpeg, args.true_peak_ceiling)
        print('Raw DSP report:\n' + dsp)
        print('Output safety: safe-gain')
        print(f"True peak ceiling: {r['true_peak_ceiling_dbtp']} dBTP")
        for name in ('before', 'after'):
            m = r[name]
            tp = '-inf (verified silence)' if m['true_peak_silence'] else m['true_peak_dbtp']
            sp = '-inf' if m['sample_peak'] == 0 else m['sample_peak_dbfs']
            print(f'{name} true peak: {tp} dBTP; sample peak: {sp} dBFS')
        print(f"Applied gain: {r['applied_gain_db']} dB; attenuation required: {r['attenuation_required']}")
        print('Final output verification: PASS')
        print('Safety report JSON: ' + json.dumps(r, ensure_ascii=True, allow_nan=False))
        return 0
    except Exception as e:
        print(f'Output safety FAILED: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
