"""Lock metadata/hash for user-supplied local fragments; leave missing entries pending."""
import argparse
import json
from pathlib import Path
import re
import sys

from audio_experiment import command, float_wav_info, json_new, sha256


def inspect(manifest, output, cli):
    manifest = Path(manifest).resolve()
    data = json.loads(manifest.read_text(encoding='utf-8'))
    ids = [entry['id'] for entry in data['fragments']]
    if len(set(ids)) != len(ids) or any(not re.fullmatch(r'NX\d{3}', identity) for identity in ids):
        raise ValueError('Fragment IDs must be unique NXddd identifiers')
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'decoded').mkdir()
    data['manifest_sha256'] = sha256(manifest)
    data['cli_sha256'] = sha256(cli)
    for entry in data['fragments']:
        source = entry.get('local_fragment_path')
        for field in ('sample_rate', 'channels', 'duration_seconds', 'source_sha256'):
            entry[field] = None
        entry['status'] = 'PENDING_USER_AUDIO'
        if not source:
            continue
        try:
            path = Path(source)
            if not path.is_absolute():
                path = manifest.parent / path
            path = path.resolve()
            entry['local_fragment_path'] = str(path)
            before = sha256(path)
            decoded = output / 'decoded' / f"{entry['id']}.wav"
            command([cli, '--input', path, '--output', decoded, '--mode', 'bypass'])
            info = float_wav_info(decoded)
            if sha256(path) != before:
                raise ValueError('Source changed during metadata inspection')
            entry.update(sample_rate=info['sample_rate'], channels=info['channels'],
                         duration_seconds=info['duration_seconds'], source_sha256=before,
                         decoded_frames=info['frames'], decoded_sha256=sha256(decoded))
            entry['status'] = 'READY' if 15 <= info['duration_seconds'] <= 30 else 'DURATION_REVIEW'
        except Exception as e:
            entry.update(status='UNAVAILABLE', error=str(e))
    json_new(output / 'corpus-lock.json', data)
    return data


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', required=True, type=Path)
    p.add_argument('--output-dir', required=True, type=Path)
    p.add_argument('--cli', type=Path, default=Path(__file__).resolve().parents[1] / 'build/nyxen-cli.exe')
    args = p.parse_args()
    try:
        data = inspect(args.manifest, args.output_dir, args.cli)
        counts = {status: sum(x['status'] == status for x in data['fragments'])
                  for status in {x['status'] for x in data['fragments']}}
        print(json.dumps(counts, indent=2))
    except Exception as e:
        print(f'Error: {e}', file=sys.stderr)
        sys.exit(1)
