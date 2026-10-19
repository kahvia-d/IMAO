"""Compare a native port against frozen single-image evidence; never revise truth."""
from pathlib import Path
import argparse
import hashlib
import json

def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))

def pose(selected):
    return None if selected is None else (selected['xy'], selected['z'])

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', required=True)
    p.add_argument('--decoder', required=True)
    p.add_argument('--inference', required=True)
    p.add_argument('--preprocessing', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args()
    reference = {r['id']: r['primarySelected'] for r in read(args.reference)['rows']}
    result = {'scope': 'Native port/export consistency on previously observed inputs, not new-session accuracy.',
              'referenceFrames': len(reference), 'checks': {}, 'files': {}}
    for name in ('reference', 'decoder', 'inference', 'preprocessing'):
        path = Path(getattr(args, name))
        result['files'][name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    for name in ('decoder', 'inference'):
        rows = read(getattr(args, name))
        assert len(rows) == len(reference) and {r['id'] for r in rows} == reference.keys()
        differences = [{'id': r['id'], 'actual': r['selected'], 'reference': reference[r['id']]}
                       for r in rows if pose(r['selected']) != pose(reference[r['id']])]
        result['checks'][name] = {'frames': len(rows), 'sameXYAndOptionalZ': len(rows)-len(differences),
                                 'differences': differences}
    preprocessing = read(args.preprocessing)
    assert preprocessing and all(r['samePixels'] is True for r in preprocessing)
    result['checks']['preprocessing'] = {'frames': len(preprocessing), 'exactRawAndContrastPixels': len(preprocessing)}
    Path(args.output).write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    assert all(not result['checks'][name]['differences'] for name in ('decoder', 'inference'))
    print(json.dumps(result['checks'], ensure_ascii=False))

if __name__ == '__main__':
    main()
