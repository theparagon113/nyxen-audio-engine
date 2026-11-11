"""CLI contract and independently encoded synthetic fixtures. Stdlib only.
FFmpeg is optional and used only to generate MP3 test inputs, never by Nyxen.
"""
import hashlib
import math
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import wave

cli, base, ffmpeg, tester = sys.argv[1:]
folder = Path(base).absolute() / str(uuid.uuid4())
folder.mkdir(parents=True)


def run(args, ok=True):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"exit={result.returncode}: {args}\n{result.stdout}\n{result.stderr}")
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def process(source, target):
    before = digest(source)
    result = run([cli, '--input', source, '--output', target, '--mode', 'bypass'])
    assert digest(source) == before
    for field in ('Input format:', 'Sample rate:', 'Channels:', 'Duration:', 'Input peak:',
                  'Input RMS:', 'Output peak:', 'Processing time:', 'WAV IEEE float32'):
        assert field in result.stdout, field
    diff = run([tester, '--compare', source, target])
    print(f'{source.name}: {diff.stdout.strip()}')
    return result.stdout


run([cli, '--help'])
for args in ([], ['--mode', 'eq'], ['--unknown', 'x'], ['--input'],
             ['--input', 'a', '--input', 'b']):
    run([cli, *args], ok=False)

for channels in (1, 2):
    for rate in (44100, 48000, 96000):
        source = folder / f'señal estéreo-{channels}-{rate}.wav'
        payload = bytearray()
        for frame in range(12347):
            for ch in range(channels):
                payload.extend(struct.pack('<h', round(20000 * math.sin(2 * math.pi * (997 + ch * 503) * frame / rate))))
        with wave.open(str(source), 'wb') as writer:
            writer.setnchannels(channels)
            writer.setsampwidth(2)
            writer.setframerate(rate)
            writer.writeframes(payload)
        target = folder / f'bypass-{channels}-{rate}.wav'
        process(source, target)
        before = digest(target)
        run([cli, '--input', source, '--output', target, '--mode', 'bypass'], ok=False)
        assert digest(target) == before
        run([cli, '--input', source, '--output', source, '--mode', 'bypass'], ok=False)

mp3_cases = 0
if ffmpeg and Path(ffmpeg).is_file():
    for channels in (1, 2):
        for rate in (44100, 48000):  # MP3 does not encode 96 kHz
            source = folder / f'señal estéreo-{channels}-{rate}.wav'
            mp3 = folder / f'mp3-{channels}-{rate}.mp3'
            run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-i', source,
                 '-c:a', 'libmp3lame', '-b:a', '192k', mp3])
            report = process(mp3, folder / f'mp3-bypass-{channels}-{rate}.wav')
            assert 'Input format: MP3' in report
            assert f'Sample rate: {rate} Hz' in report
            assert f'Channels: {channels}' in report
            mp3_cases += 1
    print('MP3 VERIFIED: 4 cases (44.1/48 kHz, mono/stereo), FFmpeg libmp3lame synthetic fixtures')
else:
    print('MP3 PENDING: FFmpeg unavailable; no MP3 support claim')

bad = folder / 'broken.flac'
bad.write_bytes(b'fLaC\x00\x00\x00\x22broken')
run([cli, '--input', bad, '--output', folder / 'bad.wav', '--mode', 'bypass'], ok=False)
badmp3 = folder / 'broken.mp3'
badmp3.write_bytes(b'not an mp3 file')
run([cli, '--input', badmp3, '--output', folder / 'bad.wav', '--mode', 'bypass'], ok=False)
assert not (folder / 'bad.wav').exists()
assert not list(folder.glob('.nyxen-*.tmp'))
print(f'PASS CLI: 6 WAV + {mp3_cases} MP3 conversions, argument/errors/overwrite/Unicode checks')
print(f'Artifacts: {folder}')
