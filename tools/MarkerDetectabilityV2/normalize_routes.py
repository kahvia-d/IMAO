"""Normalize explicit human-reviewed regional reports; never infer confirmations."""
import json
from pathlib import Path

BASE = Path(__file__).resolve().parent
REVIEWED = {'reviewed-route-and-guide', 'route-samples-reviewed',
            'route-and-guide-visually-reviewed', 'route-visually-reviewed-unresolved',
            'route-source-guide-reviewed'}


def main():
    rows = {}
    coverage = {}
    for name, key in [('early', 'entries'), ('roy', 'rows'), ('rinascita', 'members'),
                      ('huanglong-late', 'members'), ('huanglong-p11', 'members')]:
        path = BASE / f'route-review-{name}.json'
        if not path.exists():
            continue
        data = json.loads(path.read_text('utf-8'))
        count = 0
        for source in data[key]:
            if source.get('status', source.get('reviewStatus')) not in REVIEWED:
                continue
            row = dict(source) if name == 'early' else {
                'eventId': source['eventId'], 'member': source['member'],
                'status': 'reviewed-route-and-guide',
                'decision': 'confirm' if source.get('proposal') in
                    ('confirm', 'confirm-proposed-for-parent-review') else 'retain-unresolved',
                'analysis': ' '.join(source.get('support', [])),
                'evidence': [f'tools/MarkerDetectabilityV2/{path.name}'] +
                    source.get('evidencePaths', source.get('evidence', [])),
                'counterEvidence': source.get('counterEvidence', []),
                'limitations': source.get('gaps', source.get('missingEvidence', [])),
                'rejectedPointIds': source.get('rejectedPointIds', []),
                'exclusionReversals': source.get('exclusionReversals', []),
            }
            if name != 'early':
                identity = source.get('proposedIdentity') or {
                    k: source[k] for k in ('stateId', 'pointId') if k in source}
                if not identity:
                    identity = next((c for c in source.get('candidates', [])
                                     if c.get('status') == 'proposed-confirmed'), {})
                for k in ('stateId', 'pointId'):
                    if k in identity:
                        row[k] = identity[k]
                if row['decision'] != 'confirm':
                    row['analysis'] += ' ' + ' '.join(row['limitations'])
            if row['eventId'] == 'BV1pa9RBtEPA:p1:e010':
                row.update(decision='confirm', stateId='909', pointId='1500192871343808512')
                row['analysis'] += ' 父任务已独立目视核对北侧单石环、东南双环、西侧弯路及三座长椅幽灵/岩柱，认可手工地形与攻略匹配。'
                row['evidence'].append('out/detectability-collection/BV1pa9RBtEPA/guides/guide-909-1500192871343808512-1.png')
            member_key = (row['eventId'], row['member'])
            if member_key in rows:
                raise ValueError(f'Duplicate reviewed member: {member_key}')
            rows[member_key] = row
            count += 1
        coverage[name] = count
    # Explicit later parent review supersedes these regional unresolved proposals.
    for path in (BASE / 'route-review-trains.json', BASE / 'route-review-shrines.json',
                 BASE / 'route-review-laguna-parent.json'):
        if not path.exists():
            continue
        for row in json.loads(path.read_text('utf-8'))['entries']:
            key = (row['eventId'], row['member'])
            assert key in rows, key
            row['supersedes'] = 'route-review-rinascita.json'
            rows[key] = row
    result = {'coverage': coverage, 'reviewedMembers': len(rows),
              'confirmationProposals': sum(r['decision'] == 'confirm' for r in rows.values()),
              'entries': list(rows.values())}
    (BASE / 'route-followup-reviewed.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'entries'}))


if __name__ == '__main__':
    main()
