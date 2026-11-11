"""Whole CLI safety tests with real FFmpeg plus explicit failure injection."""
from array import array
import json
import math
from pathlib import Path
import subprocess
import sys
from unittest.mock import patch
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from audio_experiment import command, float_wav_info, samples, sha256, write_float_wav
import output_safety

cli, base, ffmpeg = sys.argv[1:]
folder = Path(base).resolve() / str(uuid.uuid4())
folder.mkdir(parents=True)


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def fixture(path, rate, channels, kind, duration=1.013):
    frames = int(rate * duration)
    def blocks():
        for start in range(0, frames, 8192):
            data = array('f')
            for n in range(start, min(start + 8192, frames)):
                for ch in range(channels):
                    if kind == 'silence' or ch == 1:
                        x = 0.0  # right channel silence detects crosstalk
                    elif kind == 'hot':
                        x = 0.99 * math.sin(2 * math.pi * 40 * n / rate)
                    elif kind == 'intersample':
                        x = 1.24 * math.sin(math.pi * n / 2 + math.pi / 4)
                    else:
                        x = 0.2 * math.sin(2 * math.pi * 997 * n / rate)
                    data.append(x)
            yield data
    write_float_wav(path, rate, channels, blocks(), frames)


def run_cli(args, expected=True):
    p = subprocess.run([str(x) for x in [cli, *args]], capture_output=True, text=True, encoding='utf-8', errors='replace')
    require((p.returncode == 0) == expected, p.stdout + p.stderr)
    return p


def no_temporaries():
    require(not list(folder.glob('.nyxen-*')), 'safety temporary directory/file leaked')


reports = []
for rate, channels, kind in [(44100, 1, 'quiet'), (48000, 2, 'hot'), (96000, 2, 'hot'),
                             (44100, 1, 'intersample'), (48000, 2, 'intersample'),
                             (96000, 2, 'intersample'), (48000, 1, 'silence'), (48000, 2, 'silence')]:
    source = folder / f'señal-{rate}-{channels}-{kind}.wav'
    fixture(source, rate, channels, kind)
    original_hash = sha256(source)
    mode = 'eq-reference' if kind == 'hot' else 'bypass'
    raw = folder / (source.stem + '-raw.wav')
    explicit = folder / (source.stem + '-preserve.wav')
    output = folder / (source.stem + '-safe.wav')
    run_cli(['--input', source, '--output', raw, '--mode', mode])
    preserve = run_cli(['--input', source, '--output', explicit, '--mode', mode, '--output-safety', 'preserve'])
    require('NOT REQUESTED' in preserve.stdout, 'preserve claims safety without measurement')
    # Same representation: samples must be exact; cross-format byte identity is not assumed.
    require(list(samples(raw)) == list(samples(explicit)), 'preserve changed samples')
    p = run_cli(['--input', source, '--output', output, '--mode', mode, '--output-safety', 'safe-gain', '--ffmpeg', ffmpeg])
    r = json.loads(p.stdout.split('Safety report JSON: ')[1].splitlines()[0])
    require(r['verification'] == 'PASS' and r['applied_gain_db'] <= 0, 'safety status/gain')
    require(r['before']['true_peak_valid'] and r['after']['true_peak_valid'], 'mandatory true peak not measured')
    require(output_safety.ceiling_met(r['after'], -1.0), 'final true peak ceiling (zero permitted exceedance)')
    info = float_wav_info(output)
    original = float_wav_info(raw)
    require(all(info[k] == original[k] for k in ('frames', 'sample_rate', 'channels')), 'dimensions changed')
    gain = 10 ** (r['applied_gain_db'] / 20)
    for a, b in zip(samples(raw), samples(output)):
        require(array('f', (x * gain for x in a)) == b, 'gain varies/sample indices changed')
        if channels == 2:
            require(all(x == 0 for x in b[1::2]), 'crosstalk')
    if kind in ('quiet', 'silence'):
        require(r['applied_gain_db'] == 0 and not r['attenuation_required'], 'unnecessary gain/attenuation')
    if kind == 'hot':
        require(r['before']['sample_peak'] > 1 and r['applied_gain_db'] < 0, 'over-full-scale case')
    if kind == 'intersample':
        require(r['before']['sample_peak_dbfs'] < -1 and r['before']['true_peak_dbtp'] > 0, 'intersample fixture failed')
        require(r['applied_gain_db'] < 0, 'sample-peak-only policy accepted intersample clipping')
    require(sha256(source) == original_hash, 'source altered')
    reports.append(dict(fixture=source.name, **r))
    print(f"PASS {source.name}: TP {r['before']['true_peak_dbtp']} -> {r['after']['true_peak_dbtp']} dBTP; gain {r['applied_gain_db']:.3f} dB")
    no_temporaries()

# Configurable ceiling and true peak for sub-100 ms input (LUFS irrelevant).
tiny = folder / 'tiny.wav'
fixture(tiny, 48000, 1, 'intersample', 0.017)
p = run_cli(['--input', tiny, '--output', folder / 'tiny-safe.wav', '--mode', 'bypass',
             '--output-safety', 'safe-gain', '--true-peak-ceiling', '-3', '--ffmpeg', ffmpeg])
tiny_report = json.loads(p.stdout.split('Safety report JSON: ')[1].splitlines()[0])
require(tiny_report['after']['true_peak_upper_dbtp'] <= -3 and not tiny_report['after']['integrated_valid'], 'short TP safety coupled to LUFS')
reports.append(dict(fixture='tiny-17ms', **tiny_report))

source = folder / 'señal-48000-2-hot.wav'
protected = sha256(source)
for output, extra in [(source, []), (folder / 'señal-48000-2-hot-safe.wav', []),
                      (folder / 'missing-meter.wav', ['--ffmpeg', folder / 'missing-ffmpeg.exe'])]:
    run_cli(['--input', source, '--output', output, '--mode', 'eq-reference', '--output-safety', 'safe-gain', *extra], False)
    no_temporaries()
require(sha256(source) == protected and not (folder / 'missing-meter.wav').exists(), 'failure protection')
for option, value in [('--output-safety', 'limiter'), ('--true-peak-ceiling', 'nan'),
                      ('--true-peak-ceiling', '0'), ('--true-peak-ceiling', '-1junk')]:
    run_cli(['--input', source, '--output', folder / 'invalid.wav', '--mode', 'bypass', option, value], False)

# Failures after creating/rendering temporaries must never publish an output.
real_measure = output_safety.measure
calls = 0
def failed_post_measure(*args, **kwargs):
    global calls
    calls += 1
    if calls > 1:
        raise RuntimeError('Injected measurement failure after gain/export')
    return real_measure(*args, **kwargs)
with patch.object(output_safety, 'measure', failed_post_measure):
    try:
        output_safety.protect(source, folder / 'failed-post.wav', 'eq-reference', cli, ffmpeg)
        raise AssertionError('failed post-measurement succeeded')
    except RuntimeError:
        pass
require(not (folder / 'failed-post.wav').exists(), 'failed output published')
no_temporaries()

# Successful scanner execution with an invalid TP result must also fail closed.
def invalid_tp(*args, **kwargs):
    m = real_measure(*args, **kwargs)
    m['true_peak_valid'] = False
    return m
with patch.object(output_safety, 'measure', invalid_tp):
    try:
        output_safety.protect(source, folder / 'invalid-meter.wav', 'eq-reference', cli, ffmpeg)
        raise AssertionError('invalid true peak accepted')
    except RuntimeError:
        pass
require(not (folder / 'invalid-meter.wav').exists(), 'invalid measurement output published')
no_temporaries()
broken = folder / 'corrupt.wav'
broken.write_bytes(b'RIFF broken')
run_cli(['--input', broken, '--output', folder / 'corrupt-safe.wav', '--mode', 'eq-reference',
         '--output-safety', 'safe-gain', '--ffmpeg', ffmpeg], False)
require(not (folder / 'corrupt-safe.wav').exists(), 'corrupt input output published')
no_temporaries()

# Simulate bounded meter estimate shifts; rerender from raw with one total gain.
count = 0
def shifted_measure(*args, **kwargs):
    global count
    count += 1
    m = real_measure(*args, **kwargs)
    if count == 2:
        m['true_peak_upper_dbtp'] = -0.8
    return m
with patch.object(output_safety, 'measure', shifted_measure):
    _, corrected = output_safety.protect(source, folder / 'corrected.wav', 'eq-reference', cli, ffmpeg)
require(corrected['corrections'] == 1 and corrected['applied_gain_db'] < reports[1]['applied_gain_db'], 'conservative correction path')
no_temporaries()

def never_safe(*args, **kwargs):
    m = real_measure(*args, **kwargs)
    m['true_peak_upper_dbtp'] = 1.0
    return m
with patch.object(output_safety, 'measure', never_safe):
    try:
        output_safety.protect(source, folder / 'nonconvergent.wav', 'eq-reference', cli, ffmpeg)
        raise AssertionError('nonconvergent output accepted')
    except RuntimeError:
        pass
require(not (folder / 'nonconvergent.wav').exists(), 'nonconvergent output published')
no_temporaries()
with (folder / 'safety-test-report.json').open('x', encoding='utf-8') as s:
    json.dump(dict(status='PASS', cases=reports), s, indent=2, allow_nan=False)
print('PASS output safety: preserve, mono/stereo, silence, >FS, intersample, short input, constant gain, failures/cleanup/corrections')
print(f'Artifacts: {folder}')
