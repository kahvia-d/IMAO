import unittest
import copy
import contract
from contract import validate_timeline, can_start_matching, can_start_overview


class TimelineContractTests(unittest.TestCase):
    def timeline(self):
        return {'bvid':'BVsample','part':1,'duration':60,'reviewStatus':'reviewed',
                'audioReviewStatus':'reviewed','progressBarReviewStatus':'reviewed',
                'events':[{'eventId':'BVsample:p1:e001','ordinal':1,'start':5,'end':40,
                           'rewards':[{'member':i,'matchingReview':{'analysis':'Compared route and box appearance; identity remains unresolved.'}} for i in (1,2,3)]}],
                'nonEventIntervals':[[0,5,'intro'],[40,60,'travel']]}

    def test_three_rewards_do_not_become_three_events(self):
        t=self.timeline()
        self.assertEqual([],validate_timeline(t))
        self.assertEqual(1,len(t['events']))
        self.assertEqual(3,len(t['events'][0]['rewards']))

    def test_gap_is_not_silently_reviewed(self):
        t=self.timeline();t['events'][0]['start']=9
        self.assertTrue(any('coverage gap' in e for e in validate_timeline(t)))

    def test_order_must_follow_video_not_overview_number(self):
        t=self.timeline();t['events']=[dict(t['events'][0],start=20,end=30),
                                     dict(t['events'][0],eventId='BVsample:p1:e002',ordinal=2,start=5,end=20)]
        self.assertTrue(any('order' in e for e in validate_timeline(t)))

    def test_matching_requires_every_manifest_part_and_audio_review(self):
        t=self.timeline();manifest=[('BVsample',1),('BVsample',2)]
        self.assertFalse(can_start_matching(manifest,[t]))
        t['audioReviewStatus']='machine-transcript-only'
        self.assertFalse(can_start_matching([('BVsample',1)],[t]))

    def test_overview_requires_completed_first_pass_for_every_reward(self):
        t=self.timeline()
        self.assertFalse(can_start_overview([('BVsample',1)],[t]))
        for r in t['events'][0]['rewards']:r['firstPassStatus']='reviewed-unresolved'
        self.assertTrue(can_start_overview([('BVsample',1)],[t]))

    def test_phase_times_reject_nonfinite_nonnumeric_and_outside_media(self):
        for field in ('triggerSeconds','appearanceSeconds','collectionSeconds'):
            for value in (float('nan'),float('inf'),-1,61,'12',True):
                with self.subTest(field=field,value=value):
                    t=self.timeline();t['events'][0]['rewards'][0][field]=value
                    self.assertTrue(validate_timeline(t))

    def test_existing_box_may_appear_before_trigger_but_not_after_collection(self):
        t=self.timeline();r=t['events'][0]['rewards'][0]
        r.update(appearanceSeconds=2,triggerSeconds=20,collectionSeconds=30)
        self.assertEqual([],validate_timeline(t))
        r['collectionSeconds']=1
        self.assertTrue(validate_timeline(t))

    def test_unknown_phase_times_remain_nullable(self):
        t=self.timeline();t['events'][0]['rewards'][0].update(triggerSeconds=None,appearanceSeconds=None,collectionSeconds=None)
        self.assertEqual([],validate_timeline(t))

    def test_event_id_must_encode_source_part_and_ordinal(self):
        for value in ('BVother:p1:e001','BVsample:p2:e001','BVsample:p1:e002','freeform'):
            t=self.timeline();t['events'][0]['eventId']=value
            self.assertTrue(validate_timeline(t))

    def test_reward_members_must_be_contiguous_unique_in_order(self):
        for values in ((1,1,3),(1,3,4),(2,1,3),(True,2,3)):
            t=self.timeline()
            for r,v in zip(t['events'][0]['rewards'],values):r['member']=v
            self.assertTrue(validate_timeline(t))

    def test_overview_rejects_status_without_reasoned_review(self):
        for analysis in (None,'', '   ', [], {}):
            t=self.timeline()
            for r in t['events'][0]['rewards']:
                r.update(firstPassStatus='reviewed-unresolved',matchingReview={'analysis':analysis})
            self.assertFalse(can_start_overview([('BVsample',1)],[t]))

    def confirmed_timeline(self):
        t=self.timeline();t['events'][0]['rewards']=t['events'][0]['rewards'][:1]
        def evidence(source,locator):
            return {'source':source,'locator':locator,'reason':'Independent comparison supports the candidate.','role':'support'}
        r=t['events'][0]['rewards'][0]
        r.update(identityStatus='confirmed',firstPassStatus='reviewed-confirmed',stateId='region-a',pointId='point-1',
                 detectability='undetectable',matchingReview={
                     'analysis':'Route, mechanism and chest agree with this candidate.',
                     'positionEvidence':[evidence('map-snapshot','north of bridge')],
                     'videoEvidence':[dict(evidence('BVsample:p1','frame-30'),start=29,end=31)],
                     'guideEvidence':[evidence('guide-url','section-4')],
                     'exclusionEvidence':[],
                     'attributeReview':{'status':'reviewed','analysis':'Detector comparison is visible before activation.',
                                        'detectability':'undetectable','evidence':[evidence('detector-comparison','before-trigger')]}})
        return t

    def matching_validator(self):
        self.assertTrue(callable(getattr(contract,'validate_matching',None)), 'independent matching validator required')
        return contract.validate_matching

    def test_confirmed_requires_string_ids_and_all_independent_evidence(self):
        validate=self.matching_validator();t=self.confirmed_timeline()
        self.assertEqual([],validate([t]))
        for field in ('stateId','pointId'):
            for value in (None,12,'   '):
                bad=copy.deepcopy(t);bad['events'][0]['rewards'][0][field]=value
                self.assertTrue(validate([bad]))
        for field in ('analysis','positionEvidence','videoEvidence','guideEvidence','attributeReview'):
            bad=copy.deepcopy(t);del bad['events'][0]['rewards'][0]['matchingReview'][field]
            self.assertTrue(validate([bad]))

    def test_exclusion_cannot_be_reused_as_support(self):
        validate=self.matching_validator();t=self.confirmed_timeline();review=t['events'][0]['rewards'][0]['matchingReview']
        review['exclusionEvidence']=[dict(review['guideEvidence'][0],role='exclusion')]
        self.assertTrue(validate([t]))
        review['exclusionEvidence']=[];review['guideEvidence'][0]['role']='exclusion'
        self.assertTrue(validate([t]))

    def test_video_evidence_must_reference_source_part_and_bounded_time(self):
        validate=self.matching_validator()
        for patch in ({'source':'BVother:p1'},{'start':float('nan')},{'end':61},{'start':32,'end':31}):
            t=self.confirmed_timeline();t['events'][0]['rewards'][0]['matchingReview']['videoEvidence'][0].update(patch)
            self.assertTrue(validate([t]))

    def test_attribute_review_is_required_even_after_identity_confirmation(self):
        validate=self.matching_validator()
        for patch in ({'status':'pending'},{'analysis':''},{'evidence':[]},{'detectability':'detectable'}):
            t=self.confirmed_timeline();t['events'][0]['rewards'][0]['matchingReview']['attributeReview'].update(patch)
            self.assertTrue(validate([t]))

    def test_positive_export_omits_unknown_and_unresolved_and_does_not_mutate(self):
        self.assertTrue(callable(getattr(contract,'export_positives',None)), 'pure positive exporter required')
        t=self.confirmed_timeline();before=copy.deepcopy(t)
        result=contract.export_positives([t])
        self.assertEqual(1,len(result));self.assertEqual('point-1',result[0]['pointId'])
        self.assertEqual(before,t)
        r=t['events'][0]['rewards'][0]
        r['detectability']='unknown';r['matchingReview']['attributeReview']['detectability']='unknown'
        self.assertEqual([],contract.export_positives([t]))
        t=self.timeline()
        self.assertEqual([],contract.export_positives([t]))

    def test_invalid_confirmation_blocks_export_and_overview(self):
        validate=self.matching_validator();t=self.confirmed_timeline()
        del t['events'][0]['rewards'][0]['matchingReview']['guideEvidence']
        self.assertTrue(validate([t]))
        self.assertFalse(can_start_overview([('BVsample',1)],[t]))
        self.assertTrue(callable(getattr(contract,'export_positives',None)))
        with self.assertRaises(ValueError):contract.export_positives([t])

if __name__=='__main__':unittest.main()
