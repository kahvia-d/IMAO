"""Compile the maintained positive catalog; never infer labels or read video caches."""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def compile_catalog(catalog, valid):
    if type(catalog.get('schemaVersion')) is not int or catalog['schemaVersion'] != 1:
        raise ValueError('Unsupported catalog version')
    states = {}
    seen = set()
    for row in catalog['points']:
        key = (row.get('stateId'), row.get('pointId'))
        if any(not isinstance(v, str) or not re.fullmatch(r'[0-9]+', v) for v in key):
            raise ValueError('IDs must be decimal strings')
        if key in seen or key not in valid:
            raise ValueError(f'Duplicate or missing point: {key}')
        if not row.get('sources') or not row.get('reviewBasis'):
            raise ValueError(f'Missing review provenance: {key}')
        if not isinstance(row['reviewBasis'], list) or any(not isinstance(v,str) or not v.strip() for v in row['reviewBasis']):
            raise ValueError(f'Invalid review basis: {key}')
        if not isinstance(row['sources'],list):
            raise ValueError(f'Invalid sources: {key}')
        for source in row['sources']:
            if (not isinstance(source,dict) or not isinstance(source.get('bvid'),str)
                or not source['bvid'].startswith('BV') or type(source.get('part')) is not int or source['part']<1
                or not isinstance(source.get('eventId'),str)
                or not re.fullmatch(re.escape(f"{source['bvid']}:p{source['part']}:e")+r'[0-9]+',source['eventId'])):
                raise ValueError(f'Invalid video event provenance: {key}')
        seen.add(key)
        states.setdefault(key[0], []).append(key[1])
    return dict(schemaVersion=1, states={k: sorted(states[k]) for k in sorted(states)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    valid = {(p.stem.removeprefix('state-'), str(x['id']))
             for p in (ROOT/'Assets/KuroMap/states').glob('state-*.json')
             for g in json.loads(p.read_text('utf-8-sig')) for x in g['location']}
    catalog = json.loads((Path(__file__).parent/'catalog.json').read_text('utf-8'))
    result = compile_catalog(catalog, valid)
    output = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    target = ROOT/'Assets/KuroMap/marker-detectability.json'
    if args.check:
        if not target.exists() or target.read_text('utf-8') != output:
            raise ValueError('Generated marker-detectability.json differs; run generate.py')
    else:
        target.write_text(output, encoding='utf-8')
    print(f"Validated {sum(map(len, result['states'].values()))} identities")


if __name__ == '__main__':
    main()
