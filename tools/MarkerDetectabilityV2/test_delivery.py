"""Delivery gates protect string identities, unique exports and evidence integrity."""
import copy
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
import test_contract
import delivery

class DeliveryTests(unittest.TestCase):
    def fixture(self):
        return test_contract.TimelineContractTests().confirmed_timeline()

    def test_group_delivery_keeps_set_provenance_and_removes_pending(self):
        from test_group_identity import GroupIdentityTests
        t=self.fixture();e=t['events'][0];original=copy.deepcopy(e['rewards'][0])
        e['rewards']=[dict(copy.deepcopy(original), member=i, identityStatus='pending', firstPassStatus='reviewed-unresolved') for i in (1,2)]
        for r in e['rewards']:
            r.pop('pointId',None);r.pop('stateId',None)
        e['identityGroups']=GroupIdentityTests().event()['identityGroups']
        result=delivery.summarize([('BVsample',1)],[t])
        self.assertEqual(2,len(result['positives']))
        self.assertEqual([],result['unresolved'])
        self.assertEqual(2,result['counts']['group-confirmed'])
        self.assertTrue(all(x['sources'][0]['member'] is None for x in result['positives']))

    def test_incomplete_first_pass_blocks_delivery(self):
        t=self.fixture();t['events'][0]['rewards'][0].pop('firstPassStatus')
        with self.assertRaises(ValueError):delivery.summarize([('BVsample',1)],[t])

    def test_duplicates_keep_all_provenance_but_one_identity(self):
        t=self.fixture();e=copy.deepcopy(t['events'][0]);e['eventId']='BVsample:p1:e002';e['ordinal']=2;t['events'].append(e)
        result=delivery.summarize([('BVsample',1)],[t])
        self.assertEqual(1,len(result['positives']))
        self.assertEqual(2,len(result['positives'][0]['sources']))
        self.assertIsInstance(result['positives'][0]['pointId'],str)

    def test_unknown_attribute_not_exported_and_categorized(self):
        t=self.fixture();r=t['events'][0]['rewards'][0];r['detectability']='unknown';r['matchingReview']['attributeReview']['detectability']='unknown'
        result=delivery.summarize([('BVsample',1)],[t])
        self.assertEqual([],result['positives']);self.assertIn('attribute-unclear',result['unresolved'][0]['categories'])

    def test_missing_local_evidence_or_unknown_identity_blocks(self):
        t=self.fixture()
        with TemporaryDirectory() as root:
            errors=delivery.audit_references(Path(root),[t],{('region-a','point-1')})
            self.assertTrue(any('missing evidence' in x for x in errors))
            errors=delivery.audit_references(Path(root),[t],set())
            self.assertTrue(any('unknown point identity' in x for x in errors))

    def test_explicit_missing_locator_cannot_fall_back_to_other_frame(self):
        t=self.fixture()
        with TemporaryDirectory() as folder:
            root=Path(folder);(root/'other.jpg').touch()
            e=t['events'][0];e['evidenceFrames']=['other.jpg']
            e['rewards'][0]['matchingReview']['videoEvidence'][0]['locator']='missing-specific-frame.jpg'
            self.assertTrue(any('missing-specific-frame.jpg' in x for x in delivery.audit_references(root,[t],{('region-a','point-1')})))

    def test_conflicting_confirmations_block_positive_export(self):
        t=self.fixture();e=copy.deepcopy(t['events'][0]);e.update(eventId='BVsample:p1:e002',ordinal=2)
        r=e['rewards'][0];r['detectability']='detectable';r['matchingReview']['attributeReview']['detectability']='detectable';t['events'].append(e)
        result=delivery.summarize([('BVsample',1)],[t])
        self.assertEqual([],result['positives']);self.assertEqual(1,len(result['identityConflicts']))
        self.assertEqual(2,len(result['identityConflicts'][0]['sources']))

    def test_overview_requires_each_part_review_and_evidence(self):
        incomplete={'status':'reviewed','parts':[{'bvid':'BVsample','part':1,'status':'pending'}]}
        self.assertTrue(delivery.validate_overview([('BVsample',1)],incomplete))
        incomplete['parts'][0]['status']='reviewed'
        self.assertTrue(delivery.validate_overview([('BVsample',1)],incomplete))

if __name__=='__main__':unittest.main()
