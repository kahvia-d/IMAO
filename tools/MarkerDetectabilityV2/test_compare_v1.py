import unittest
from compare_v1 import compare

class ComparisonTests(unittest.TestCase):
    def test_time_overlap_does_not_claim_reassignment(self):
        old={'targets':[dict(targetKey='old',bvid='BVx',part=1,identityStatus='confirmed',stateId=8,pointId='111',segmentSecondsApprox=[0,10])]}
        timelines=[dict(bvid='BVx',part=1,events=[dict(eventId='BVx:p1:e001',start=0,end=10,rewards=[dict(member=1,identityStatus='confirmed',stateId='8',pointId='222',detectability='unknown')])])]
        result=compare(old,timelines)
        self.assertEqual('not-reconfirmed',result['oldTargetCoverage'][0]['change'])
        self.assertEqual([],result['oldTargetCoverage'][0]['exactIdentitySources'])
        self.assertEqual(1,len(result['oldTargetCoverage'][0]['chronologicalOverlaps']))
        self.assertEqual({'revoked-confirmation','new-confirmation'},{x['change'] for x in result['identityChanges']})

    def test_string_identity_survives_member_reordering(self):
        pid='900719925474099312345'
        old={'targets':[dict(targetKey='old',bvid='BVx',part=1,identityStatus='confirmed',stateId=8,pointId=pid)]}
        timelines=[dict(bvid='BVx',part=1,events=[dict(eventId='BVx:p1:e001',start=0,end=10,rewards=[dict(member=2,identityStatus='confirmed',stateId='8',pointId=pid,detectability='unknown')])])]
        result=compare(old,timelines)
        self.assertEqual('retained',result['identityChanges'][0]['change'])
        self.assertEqual(pid,result['identityChanges'][0]['pointId'])
        self.assertEqual(2,result['identityChanges'][0]['newSources'][0]['member'])

if __name__=='__main__':unittest.main()
