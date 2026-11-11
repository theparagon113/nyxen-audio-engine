"""Shared offline experiment helpers. Python stdlib; FFmpeg only measures.
No playback, normalization filters, compressors, limiters, or sample-rate conversion.
"""
from array import array
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys


def command(args):
    args = [str(x) for x in args]
    p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'Command failed ({p.returncode}): {args}\n{p.stderr}\n{p.stdout}')
    return p


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def json_new(path, data):
    with open(path, 'x', encoding='utf-8') as stream:
        json.dump(data, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write('\n')


def float_wav_info(path):
    """Only reads the float32 RIFF WAVs emitted by Nyxen or these helpers."""
    path = Path(path)
    with path.open('rb') as s:
        header = s.read(12)
        if len(header) != 12 or header[:4] != b'RIFF' or header[8:] != b'WAVE':
            raise ValueError('Expected RIFF WAV')
        end = struct.unpack_from('<I', header, 4)[0] + 8
        if end > path.stat().st_size:
            raise ValueError('Truncated WAV')
        fmt, data = None, None
        while s.tell() < end:
            chunk = s.read(8)
            if len(chunk) != 8:
                raise ValueError('Truncated WAV chunk')
            name, size = struct.unpack('<4sI', chunk)
            start = s.tell()
            if start + size + size % 2 > end:
                raise ValueError('Invalid WAV chunk size')
            if name == b'fmt ':
                f = s.read(size)
                if len(f) < 16:
                    raise ValueError('Invalid fmt')
                tag, channels, rate, byte_rate, align, bits = struct.unpack_from('<HHIIHH', f)
                if tag == 0xfffe:
                    if len(f) < 40 or f[24:40] != bytes.fromhex('0300000000001000800000aa00389b71'):
                        raise ValueError('Expected extensible IEEE float32')
                    tag = 3
                if tag != 3 or bits != 32 or channels not in (1, 2) or align != channels * 4 or byte_rate != rate * align:
                    raise ValueError('Expected mono/stereo float32 WAV')
                fmt = (rate, channels, align)
            if name == b'data':
                data = (start, size)
            s.seek(start + size + size % 2)
        if fmt is None or data is None or data[1] % fmt[2] or not data[1]:
            raise ValueError('Missing/invalid WAV data')
        return dict(sample_rate=fmt[0], channels=fmt[1], frames=data[1] // fmt[2],
                    duration_seconds=(data[1] // fmt[2]) / fmt[0], data_offset=data[0], data_bytes=data[1])


def samples(path, info=None):
    info = info or float_wav_info(path)
    with open(path, 'rb') as s:
        s.seek(info['data_offset'])
        remaining = info['data_bytes']
        while remaining:
            payload = s.read(min(remaining, 262144))
            if not payload or len(payload) % 4:
                raise ValueError('Short WAV read')
            remaining -= len(payload)
            values = array('f')
            values.frombytes(payload)
            if sys.byteorder != 'little':
                values.byteswap()
            yield values


def sample_stats(path):
    peak, squares, count = 0.0, 0.0, 0
    for block in samples(path):
        if any(not math.isfinite(x) for x in block):
            raise ValueError('Nonfinite audio')
        peak = max(peak, max(abs(x) for x in block))
        squares += math.fsum(x * x for x in block)
        count += len(block)
    return dict(sample_peak=peak, sample_peak_dbfs=20 * math.log10(peak) if peak else None,
                rms=math.sqrt(squares / count), scalar_samples=count)


def write_float_wav(path, rate, channels, blocks, frames):
    size = frames * channels * 4
    if size + 36 > 0xffffffff:
        raise ValueError('RIFF output too large')
    with open(path, 'xb') as s:
        s.write(struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', size + 36, b'WAVE', b'fmt ', 16,
                            3, channels, rate, rate * channels * 4, channels * 4, 32, b'data', size))
        count = 0
        for values in blocks:
            block = array('f', values)
            count += len(block)
            if any(not math.isfinite(x) for x in block):
                raise ValueError('Nonfinite export')
            if sys.byteorder != 'little':
                block.byteswap()
            s.write(block.tobytes())
        if count != frames * channels:
            raise ValueError('Sample count changed')


def constant_gain(source, target, gain_db):
    info = float_wav_info(source)
    linear = 10 ** (gain_db / 20)
    blocks = ([x * linear for x in block] for block in samples(source, info))
    write_float_wav(target, info['sample_rate'], info['channels'], blocks, info['frames'])


def measure(path, ffmpeg, log_path):
    # Analyze only: the filter output goes to null. No analysis-resampled audio is saved.
    args = [ffmpeg, '-nostdin', '-hide_banner', '-nostats', '-i', path, '-map', '0:a:0',
            '-af', 'ebur128=peak=sample+true:metadata=1:dualmono=false,ametadata=print', '-f', 'null', '-']
    result = command(args)
    log = result.stderr
    with open(log_path, 'x', encoding='utf-8') as s:
        s.write(log)
    metadata = re.findall(r'lavfi\.r128\.I=([-+\d.einf]+)', log)
    summaries = list(re.finditer(r'Summary:', log))
    summary = log[summaries[-1].start():] if summaries else ''
    loudness = re.search(r'Integrated loudness:\s*I:\s*([-+\d.einf]+) LUFS', summary)
    true_peak = re.search(r'True peak:\s*Peak:\s*([-+\d.einf]+) dBFS', summary)
    if not loudness or not true_peak:
        raise RuntimeError('FFmpeg did not provide integrated loudness summary AND true peak')
    integrated = float(metadata[-1]) if metadata else None
    if integrated is not None and math.isfinite(integrated) and abs(integrated - float(loudness.group(1))) > 0.051:
        raise RuntimeError('Final integrated metadata and summary disagree; cannot certify loudness')
    tp = float(true_peak.group(1))  # final summary includes whole-stream peak, 0.1 dB resolution
    info = float_wav_info(path)
    stats = sample_stats(path)
    valid = (info['duration_seconds'] >= 0.4 and integrated is not None and math.isfinite(integrated)
             and integrated > -69.999 and math.isfinite(tp) and stats['sample_peak'] > 0)
    # Silence has valid TP=-inf, even though integrated loudness is invalid.
    # Still require an actually executed FFmpeg scan and its explicit -inf result.
    silent = stats['sample_peak'] == 0 and tp == -math.inf
    tp_valid = silent or (math.isfinite(tp) and stats['sample_peak'] > 0)
    return dict(**info, **stats, integrated_lufs=integrated if valid else None,
                integrated_valid=valid, true_peak_dbtp=tp if math.isfinite(tp) else None,
                true_peak_upper_dbtp=max(tp + 0.05, stats['sample_peak_dbfs']) if math.isfinite(tp) and stats['sample_peak'] else None,
                true_peak_valid=tp_valid, true_peak_silence=silent,
                measurement_command=[str(x) for x in args], measurement_log=str(Path(log_path).resolve()))
