"""Real loudness/true-peak measurement, file pipeline EQ and blind-session tests."""
from array import array
import json
import math
from pathlib import Path
import struct
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from audio_experiment import command, float_wav_info, samples, sha256, write_float_wav
from prepare_comparison import prepare
from generate_session import generate
from inspect_corpus import inspect

cli, base, ffmpeg = sys.argv[1:]
folder = Path(base).resolve() / str(uuid.uuid4())
folder.mkdir(parents=True)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def fixture(path, rate, channels, duration, kind='program'):
    frames = int(duration * rate)
    def blocks():
        for start in range(0, frames, 8192):
            b = array('f')
            for frame in range(start, min(start + 8192, frames)):
                t = frame / rate
                for ch in range(channels):
                    if kind == 'silence':
                        value = 0
                    elif kind == 'hot':
                        value = 0.99 * math.sin(2 * math.pi * 40 * t + ch * 0.2)
                    else:
                        # Deterministic nonstationary multitone/transient fixture.
                        # 997 Hz dominates: EQ changes spectrum AND integrated loudness.
                        env = 0.65 + 0.25 * math.sin(2 * math.pi * 0.17 * t)
                        value = env * (0.16 * math.sin(2 * math.pi * (80 + ch * 7) * t)
                                       + 0.13 * math.sin(2 * math.pi * (997 + ch * 103) * t)
                                       + 0.06 * math.sin(2 * math.pi * (6200 + ch * 211) * t))
                        value += 0.04 * math.exp(-((t % 0.7) * 35)) * math.sin(2 * math.pi * 1500 * t)
                    b.append(value)
            yield b
    write_float_wav(path, rate, channels, blocks(), frames)


passed = []
for rate, channels, kind in [(44100, 1, 'program'), (48000, 2, 'program'), (96000, 2, 'program'), (48000, 2, 'hot')]:
    source = folder / f'fixture-{rate}-{channels}-{kind}.wav'
    fixture(source, rate, channels, 15.037, kind)
    before = sha256(source)
    r = prepare(source, folder / source.stem, cli, ffmpeg)
    check(r['status'] == 'PASS', str(r.get('reason')))
    check(sha256(source) == before, 'original changed')
    check(r['loudness_difference_lu'] <= 0.2, 'loudness match')
    eq_changed = False
    for mode, raw in r['raw'].items():
        matched = r['matched'][mode]
        check(raw['frames'] == matched['frames'] == int(15.037 * rate), 'identical frames')
        check(raw['sample_rate'] == matched['sample_rate'] == rate, 'sample rate')
        check(raw['channels'] == matched['channels'] == channels, 'channels')
        check(matched['sample_peak'] < 1 and matched['true_peak_upper_dbtp'] <= -1, 'listening copies safe')
        check(sha256(raw['path']) == raw['sha256'], 'raw output changed')
        gain = 10 ** (matched['gain_db'] / 20)
        for raw_block, scaled_block in zip(samples(raw['path']), samples(matched['path'])):
            expected = array('f', (x * gain for x in raw_block))
            check(expected == scaled_block, 'not precisely constant gain at same sample indices')
    for original_block, eq_block in zip(samples(r['raw']['bypass']['path']), samples(r['raw']['eq-reference']['path'])):
        eq_changed |= original_block != eq_block
    check(eq_changed, 'EQ did not process audio')
    if kind == 'hot':
        check(r['raw']['eq-reference']['sample_peak'] > 1, 'headroom boost case not exercised')
        check(r['attempts'][0]['common_attenuation_db'] > 0, 'common attenuation branch')
    passed.append(folder / source.stem / 'comparison.json')
    print(f"PASS {source.stem}: raw I={r['raw']['bypass']['integrated_lufs']}/{r['raw']['eq-reference']['integrated_lufs']} LUFS; "
          f"matched I={r['matched']['bypass']['integrated_lufs']}/{r['matched']['eq-reference']['integrated_lufs']} LUFS; "
          f"delta={r['loudness_difference_lu']:.3f} LU; TP={r['matched']['bypass']['true_peak_dbtp']}/{r['matched']['eq-reference']['true_peak_dbtp']} dBTP")

# Same file processed independently twice: per-file reset and reproducibility.
source = folder / 'fixture-48000-2-program.wav'
for index in (1, 2):
    destination = folder / f'eq-repeat-{index}.wav'
    command([cli, '--input', source, '--output', destination, '--mode', 'eq-reference'])
check(sha256(folder / 'eq-repeat-1.wav') == sha256(folder / 'eq-repeat-2.wav'), 'per-file reset/repeatability')
for args in ([cli, '--input', source, '--output', source, '--mode', 'eq-reference'],
             [cli, '--input', source, '--output', folder / 'eq-repeat-1.wav', '--mode', 'eq-reference']):
    try:
        command(args)
        raise AssertionError('overwriting accepted in EQ mode')
    except RuntimeError:
        pass

for kind, duration in [('silence', 1), ('program', 0.17)]:
    source = folder / f'invalid-{kind}.wav'
    fixture(source, 48000, 1, duration, kind)
    r = prepare(source, folder / f'invalid-{kind}', cli, ffmpeg)
    check(r['status'] == 'UNFIT' and 'matched' not in r, 'invalid loudness must not pass')
    check(not list((folder / f'invalid-{kind}').glob('matched-*')), 'invalid case produced listening audio')

for index in (1, 2):
    generate(passed, folder / f'session-{index}', 'Nyxen-M02-seed-001', 'P001')
a = json.loads((folder / 'session-1/participant/session.json').read_text(encoding='utf-8'))
b = json.loads((folder / 'session-2/participant/session.json').read_text(encoding='utf-8'))
check(a == b and len(a['trials']) == 5, 'session repeat/reproducible order')
key = json.loads((folder / 'session-1/organizer-private/answer-key.json').read_text(encoding='utf-8'))
check(len(set(t['comparison_index'] for t in key['trials'])) == 4, 'repeated comparisons')
for trial in a['trials']:
    for label in ('A', 'B'):
        check(sha256(folder / 'session-1/participant' / trial[label]) == sha256(folder / 'session-2/participant' / trial[label]), 'session assignment reproducibility')
public_text = (folder / 'session-1/participant/session.json').read_text(encoding='utf-8')
check('eq-reference' not in public_text and 'bypass' not in public_text and 'source' not in public_text, 'unblinded public metadata')
try:
    generate([folder / 'invalid-silence/comparison.json'], folder / 'invalid-session', 'seed', 'P001')
    raise AssertionError('UNFIT accepted for session')
except ValueError:
    pass
template = Path(__file__).resolve().parents[1] / 'corpus/manifest.template.json'
pending = inspect(template, folder / 'corpus-pending', cli)
check(len(pending['fragments']) == 15 and all(x['status'] == 'PENDING_USER_AUDIO' for x in pending['fragments']), 'pending corpus accurately reported')
provided = json.loads(template.read_text(encoding='utf-8'))
provided['fragments'][0]['local_fragment_path'] = str(folder / 'fixture-48000-2-program.wav')
provided_path = folder / 'corpus-provided.json'
provided_path.write_text(json.dumps(provided), encoding='utf-8')
locked = inspect(provided_path, folder / 'corpus-inspected', cli)
entry = locked['fragments'][0]
check(entry['status'] == 'READY' and entry['sample_rate'] == 48000 and entry['channels'] == 2
      and entry['source_sha256'] == sha256(folder / 'fixture-48000-2-program.wav'), 'corpus metadata/hash')
print('PASS evaluation: 4 measured pairs, invalid silence/short audio, EQ file invariants, constant gain and blind manifests')
print('PASS corpus tooling: 15 pending entries, synthetic metadata/hash inspection; musical corpus still pending')
print(f'Artifacts: {folder}')
