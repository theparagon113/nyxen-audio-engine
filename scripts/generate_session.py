"""Create anonymous A/B files and a separate organizer-only answer key."""
import argparse
import csv
import json
from pathlib import Path
import random
import shutil
import sys

from audio_experiment import json_new, sha256


def generate(comparisons, output, seed, participant, repeat_fraction=0.25):
    if not comparisons or len(set(Path(p).resolve() for p in comparisons)) != len(comparisons):
        raise ValueError('Provide unique comparison reports')
    if not 0 <= repeat_fraction <= 0.5:
        raise ValueError('repeat fraction must be 0..0.5')
    validated = []
    for p in comparisons:
        p = Path(p).resolve()
        r = json.loads(p.read_text(encoding='utf-8'))
        if r['status'] != 'PASS' or r['preset_id'] != 'eq-reference-v1' or r['loudness_difference_lu'] > 0.2:
            raise ValueError(f'Ineligible comparison: {p}')
        for version in r['matched'].values():
            if not version['integrated_valid'] or version['sample_peak'] >= 1 or version['true_peak_upper_dbtp'] > r['true_peak_ceiling_dbtp']:
                raise ValueError('Invalid matched measurements')
            if sha256(version['path']) != version['sha256']:
                raise ValueError('Matched audio hash changed')
        validated.append((p, r))
    if len({r['source_sha256'] for _, r in validated}) != len(validated):
        raise ValueError('Duplicate source audio; repeated trials must be generated explicitly')
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    public = output / 'participant'
    private = output / 'organizer-private'
    public.mkdir(); private.mkdir()
    rng = random.Random(seed)
    indices = list(range(len(validated)))
    repeats = min(len(indices), max(1, round(len(indices) * repeat_fraction))) if repeat_fraction else 0
    indices += rng.sample(indices, repeats)
    rng.shuffle(indices)
    trials, key, results = [], [], []
    counts = {}
    for number, index in enumerate(indices, 1):
        report_path, r = validated[index]
        versions = ['bypass', 'eq-reference']
        if rng.getrandbits(1):
            versions.reverse()
        trial = f'T{number:03d}'
        counts[index] = counts.get(index, 0) + 1
        mapping = {}
        for label, mode in zip(('A', 'B'), versions):
            destination = public / f'{trial}_{label}.wav'
            with open(r['matched'][mode]['path'], 'rb') as source, destination.open('xb') as target:
                shutil.copyfileobj(source, target)
            mapping[label] = dict(mode=mode, sha256=sha256(destination))
        trials.append(dict(trial_id=trial, A=f'{trial}_A.wav', B=f'{trial}_B.wav'))
        key.append(dict(trial_id=trial, comparison_index=index, occurrence=counts[index],
                        comparison_report=str(report_path), source_sha256=r['source_sha256'], labels=mapping))
        results.append(dict(trial_id=trial, preference='', strength='', comments=''))
    json_new(public / 'session.json', dict(protocol='Nyxen paired preference v1', participant_id=participant,
                                         trials=trials, choices=['A', 'B', 'none'], strength_scale=[0, 1, 2, 3]))
    with open(public / 'responses.csv', 'x', encoding='utf-8', newline='') as s:
        w = csv.DictWriter(s, fieldnames=['trial_id', 'preference', 'strength', 'comments'])
        w.writeheader(); w.writerows(results)
    json_new(private / 'answer-key.json', dict(seed=seed, python_version=sys.version,
                                              repeat_fraction=repeat_fraction, trials=key))
    return trials


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--comparison', type=Path, action='append', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--seed', required=True)
    parser.add_argument('--participant-id', required=True)
    parser.add_argument('--repeat-fraction', type=float, default=0.25)
    args = parser.parse_args()
    try:
        trials = generate(args.comparison, args.output_dir, args.seed, args.participant_id, args.repeat_fraction)
        print(f'Prepared {len(trials)} anonymous trials. Share only the participant directory.')
    except Exception as e:
        print(f'Error: {e}', file=sys.stderr)
        sys.exit(1)
