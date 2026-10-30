"""Import explicitly reviewed route proposals, retaining the prior review intact."""
import argparse
import copy
import hashlib
import json
import re
from pathlib import Path

from contract import validate_matching


def guide_matches(path, state, point):
    return bool(re.search(re.escape(f'guide-{state}-{point}') + r'(?=[.\-/#?]|$)', path))


def apply_review(reward, row, report):
    if row.get('status') != 'reviewed-route-and-guide':
        return False
    if row.get('decision') not in ('confirm', 'retain-unresolved'):
        raise ValueError('reviewed route requires an explicit decision')
    if not row.get('analysis') or not row.get('evidence'):
        raise ValueError('reviewed route requires analysis and evidence')
    digest = hashlib.sha256(('v4:' + json.dumps(row, sort_keys=True, ensure_ascii=False)).encode()).hexdigest()
    if reward.get('routeFollowup', {}).get('digest') == digest:
        return False
    review = reward.get('matchingReview', {})
    excluded = {x['locator'].replace('\\', '/') for x in
                [*review.get('exclusionEvidence', []),
                 *(g for g in review.get('guideEvidence', []) if g.get('role') == 'exclusion')]}
    reversals = row.get('exclusionReversals', [])
    if any(not r.get('reason', '').strip() or r.get('locator') not in row['evidence'] for r in reversals):
        raise ValueError('exclusion reversal needs explicit reason and re-reviewed evidence')
    reversed_paths = {r['locator'].replace('\\', '/') for r in reversals}
    def rejected(path):
        return any(guide_matches(path, row.get('stateId'), pid) for pid in row.get('rejectedPointIds', []))
    if any(p.replace('\\', '/') in excluded - reversed_paths and not rejected(p) for p in row['evidence']):
        raise ValueError('excluded evidence cannot support a route decision')
    if row['decision'] == 'confirm':
        sid, pid = row.get('stateId'), row.get('pointId')
        if not all(isinstance(v, str) and v.strip() for v in (sid, pid)):
            raise ValueError('confirmation requires string identity')
        token = f'guide-{sid}-{pid}'
        if not (any(guide_matches(g.get('locator', '') + g.get('source', ''), sid, pid) and g.get('role') == 'support'
                    for g in review.get('guideEvidence', []))
                or any(guide_matches(p, sid, pid) for p in row['evidence'])):
            raise ValueError('confirmation needs guide evidence for the actual identity')
    before = copy.deepcopy(reward)
    # Keep the original pre-route state once; revisions retain prior route decisions too.
    history = before.pop('routeFollowup', None)
    reward['routeFollowup'] = {'report': report, 'digest': digest, 'decision': row['decision'],
                              'analysis': row['analysis'], 'evidence': row['evidence'],
                              'reviewRow': copy.deepcopy(row),
                              'before': history['before'] if history else before}
    if history:
        reward['routeFollowup']['previousRevision'] = {k: v for k, v in history.items() if k != 'before'}
    review = reward['matchingReview']
    if reversals:
        review['exclusionReversals'] = copy.deepcopy(reversals)
        review['exclusionEvidence'] = [g for g in review.get('exclusionEvidence', [])
                                      if g['locator'].replace('\\', '/') not in reversed_paths]
        review['guideEvidence'] = [g for g in review.get('guideEvidence', [])
                                  if not (g.get('role') == 'exclusion' and
                                          g['locator'].replace('\\', '/') in reversed_paths)]
    review['analysis'] = row['analysis']
    review['missingEvidence'] = [] if row['decision'] == 'confirm' else [row['analysis']]
    review['routeLimitations'] = row.get('limitations', [])
    review['routeCounterEvidence'] = row.get('counterEvidence', [])
    retained_guides = []
    for item in review.get('guideEvidence', []):
        if any(guide_matches(item.get('locator', '') + item.get('source', ''), row.get('stateId'), pid)
               for pid in row.get('rejectedPointIds', [])):
            item = dict(item, role='exclusion', reason='本轮路线和画面否定该候选。' + row['analysis'])
            if item not in review.setdefault('exclusionEvidence', []):
                review['exclusionEvidence'].append(item)
        else:
            retained_guides.append(item)
    review['guideEvidence'] = retained_guides
    if row['decision'] == 'confirm':
        token = f'guide-{row["stateId"]}-{row["pointId"]}'
        matching_guides, superseded = [], []
        for item in review['guideEvidence']:
            current = guide_matches(item.get('locator', '') + item.get('source', ''), row['stateId'], row['pointId']) and (
                item.get('role') == 'support' or item.get('locator') in row['evidence'])
            (matching_guides if current else superseded).append(item)
        review['guideEvidence'] = matching_guides
        if superseded:
            review.setdefault('supersededGuideEvidence', []).extend(superseded)
    # Prior position arguments remain in routeFollowup.before, not as current claims.
    positions = review['positionEvidence'] = []
    existing = {p.get('locator') for p in positions}
    for path in [report, *row['evidence']]:
        if path not in existing:
            item = {'source': 'route-followup', 'locator': path,
                    'reason': row['analysis'], 'role': 'support'}
            if 'guide-' in path:
                if row['decision'] == 'confirm' and not guide_matches(path, row['stateId'], row['pointId']):
                    field = 'exclusionEvidence' if rejected(path) else 'diagnosticEvidence'
                    item['role'] = 'exclusion' if rejected(path) else 'diagnostic'
                    item['reason'] = '其他候选的对照证据，不支持当前ID。' + row['analysis']
                    if not any(g.get('locator') == path for g in review.setdefault(field, [])):
                        review[field].append(item)
                    existing.add(path)
                    continue
                prior = next((g for g in review['guideEvidence'] if g.get('locator') == path), None)
                if prior is None:
                    review['guideEvidence'].append(item)
                else:
                    # Explicitly re-reviewed evidence replaces a candidate role;
                    # the previous role remains in routeFollowup.before.
                    prior.update(item)
            elif path.endswith(('source-terrain.jpg', 'proposals.json')) or '/route-early/' in path and path.endswith('/route-map.png'):
                item.update(role='diagnostic', reason='自动定位提议对照，含本轮已否决的错峰；有效部分以最新审核结论为准。')
                diagnostics = review.setdefault('diagnosticEvidence', [])
                if not any(g.get('locator') == path for g in diagnostics):
                    diagnostics.append(item)
            else:
                positions.append(item)
            existing.add(path)
    if row['decision'] == 'confirm':
        reward.update(identityStatus='confirmed', firstPassStatus='reviewed-confirmed',
                      stateId=row['stateId'], pointId=row['pointId'])
    else:
        reward.update(identityStatus='pending', firstPassStatus='reviewed-unresolved')
        reward.pop('pointId', None)
        reward.pop('stateId', None)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('report', help='Explicit normalized report with entries; no automatic discovery')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    report_path = root / args.report
    data = json.loads(report_path.read_text(encoding='utf-8-sig'))
    files = list((root / 'tools/MarkerDetectabilityV2/timelines').glob('*.json'))
    timelines = {p: json.loads(p.read_text(encoding='utf-8-sig')) for p in files}
    targets = {(e['eventId'], r['member']): (p, r) for p, t in timelines.items()
               for e in t['events'] for r in e.get('rewards', [])}
    touched, seen = set(), set()
    count = 0
    for row in data['entries']:
        key = row['eventId'], row['member']
        if key in seen:
            raise ValueError(f'duplicate review {key}')
        seen.add(key)
        if row.get('status') != 'reviewed-route-and-guide':
            continue
        for path in row.get('evidence', []):
            local = (root / path).resolve()
            if not local.is_relative_to(root) or not local.is_file():
                raise ValueError(f'missing route evidence {path}')
        p, reward = targets[key]
        if apply_review(reward, row, report_path.relative_to(root).as_posix()):
            count += 1
            touched.add(p)
    errors = validate_matching(list(timelines.values()))
    if errors:
        raise ValueError('\n'.join(errors))
    for p in touched:
        p.write_text(json.dumps(timelines[p], ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'imported': count, 'timelineFiles': len(touched)}))


if __name__ == '__main__':
    main()
