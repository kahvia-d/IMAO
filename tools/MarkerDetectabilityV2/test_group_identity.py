import copy
import unittest
from group_identity import validate_groups, group_positives


class GroupIdentityTests(unittest.TestCase):
    def event(self):
        ev = lambda p: dict(source='review', locator=p, reason='reviewed', role='support')
        return dict(eventId='BV:p1:e001', rewards=[dict(member=i, identityStatus='pending',
            detectability='undetectable') for i in (1, 2)], identityGroups=[dict(
            groupId='g1', status='confirmed-set', members=[1, 2],
            points=[dict(stateId='8', pointId=p) for p in ('11','12')],
            detectability='undetectable', analysis='same two-chest reward',
            videoEvidence=[ev('source.jpg')], positionEvidence=[ev('map.jpg')],
            guideEvidence=[ev('guide-8-11.json'), ev('guide-8-12.json')])])

    def test_exports_set_without_inventing_member_mapping(self):
        e=self.event();before=copy.deepcopy(e)
        self.assertEqual([],validate_groups(e))
        out=list(group_positives([dict(bvid='BV',part=1,events=[e])]))
        self.assertEqual(['11','12'],[p['pointId'] for p in out])
        self.assertTrue(all(p['member'] is None and p['members']==[1,2] for p in out))
        self.assertEqual(before,e)

    def test_partial_duplicate_or_wrong_guide_set_rejected(self):
        for mutation in ('count','duplicate','guide','attribute'):
            e=self.event();g=e['identityGroups'][0]
            if mutation=='count':g['points'].pop()
            if mutation=='duplicate':g['points'][1]=g['points'][0]
            if mutation=='guide':g['guideEvidence'].pop()
            if mutation=='attribute':e['rewards'][0]['detectability']='unknown'
            self.assertTrue(validate_groups(e),mutation)

    def test_overlapping_group_rejected(self):
        e=self.event();e['identityGroups'].append(copy.deepcopy(e['identityGroups'][0]))
        self.assertTrue(validate_groups(e))
