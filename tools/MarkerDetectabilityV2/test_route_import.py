import copy
import unittest
from route_import import apply_review


class RouteImportTests(unittest.TestCase):
    def setUp(self):
        self.reward = {'member': 1, 'identityStatus': 'pending',
                       'firstPassStatus': 'reviewed-unresolved', 'detectability': 'undetectable',
                       'matchingReview': {'analysis': 'old gap', 'missingEvidence': ['old gap'],
                           'guideEvidence': [{'locator': 'guide-8-123-1.png', 'role': 'support'}],
                           'attributeReview': {'detectability': 'undetectable'}}}
        self.row = {'eventId': 'BV:p1:e001', 'member': 1, 'status': 'reviewed-route-and-guide',
                    'decision': 'confirm', 'stateId': '8', 'pointId': '123',
                    'analysis': 'route and guide match', 'evidence': ['route.png']}

    def test_unreviewed_cannot_promote(self):
        self.row['status'] = 'not-yet-route-reviewed'
        before = copy.deepcopy(self.reward)
        self.assertFalse(apply_review(self.reward, self.row, 'report.json'))
        self.assertEqual(before, self.reward)

    def test_preserves_history_and_policy_without_duplicate_import(self):
        self.assertTrue(apply_review(self.reward, self.row, 'report.json'))
        self.assertEqual('old gap', self.reward['routeFollowup']['before']['matchingReview']['analysis'])
        self.assertEqual('undetectable', self.reward['detectability'])
        self.assertEqual([], self.reward['matchingReview']['missingEvidence'])
        before = copy.deepcopy(self.reward)
        self.assertFalse(apply_review(self.reward, self.row, 'report.json'))
        self.assertEqual(before, self.reward)

    def test_guide_for_another_id_cannot_support_promotion(self):
        self.row['pointId'] = '999'
        with self.assertRaises(ValueError):
            apply_review(self.reward, self.row, 'report.json')
        self.assertEqual('pending', self.reward['identityStatus'])

    def test_excluded_locator_cannot_be_reused(self):
        self.reward['matchingReview']['exclusionEvidence'] = [{'locator': 'route.png'}]
        with self.assertRaises(ValueError):
            apply_review(self.reward, self.row, 'report.json')

    def test_unresolved_review_updates_reason_without_binding_candidate(self):
        self.row['decision'] = 'retain-unresolved'
        apply_review(self.reward, self.row, 'report.json')
        self.assertNotIn('pointId', self.reward)
        self.assertEqual('pending', self.reward['identityStatus'])
        self.assertEqual('route and guide match', self.reward['matchingReview']['analysis'])

    def test_reassignment_keeps_rejected_guide_out_of_support(self):
        self.row.update(pointId='999', rejectedPointIds=['123'],
                        evidence=['route.png', 'guide-8-999-1.png'])
        apply_review(self.reward, self.row, 'report.json')
        review = self.reward['matchingReview']
        self.assertEqual(['guide-8-999-1.png'], [g['locator'] for g in review['guideEvidence']])
        self.assertEqual('guide-8-123-1.png', review['exclusionEvidence'][0]['locator'])
        self.assertEqual('exclusion', review['exclusionEvidence'][0]['role'])

    def test_failed_template_comparison_is_diagnostic_not_support(self):
        self.row['evidence'].append('folder/source-terrain.jpg')
        apply_review(self.reward, self.row, 'report.json')
        review = self.reward['matchingReview']
        self.assertNotIn('folder/source-terrain.jpg', [g['locator'] for g in review['positionEvidence']])
        self.assertEqual('diagnostic', review['diagnosticEvidence'][0]['role'])

    def test_explicit_recheck_promotes_candidate_guide_and_keeps_original(self):
        self.reward['matchingReview']['guideEvidence'][0]['role'] = 'candidate'
        self.row['evidence'].append('guide-8-123-1.png')
        apply_review(self.reward, self.row, 'report.json')
        self.assertEqual('support', self.reward['matchingReview']['guideEvidence'][0]['role'])
        self.assertEqual('candidate', self.reward['routeFollowup']['before']['matchingReview']['guideEvidence'][0]['role'])

    def test_old_context_does_not_mix_with_new_confirmed_guide(self):
        self.reward['matchingReview']['guideEvidence'][0]['role'] = 'context'
        self.row['evidence'].append('guide-8-123-2.png')
        apply_review(self.reward, self.row, 'report.json')
        self.assertEqual(['guide-8-123-2.png'], [g['locator'] for g in self.reward['matchingReview']['guideEvidence']])

    def test_explicit_exclusion_reversal_preserves_original_reason(self):
        self.reward['matchingReview']['exclusionEvidence'] = [
            {'locator': 'guide-8-123-1.png', 'role': 'exclusion', 'reason': 'old scene mismatch'}]
        self.row['evidence'].append('guide-8-123-1.png')
        self.row['exclusionReversals'] = [{'locator': 'guide-8-123-1.png',
            'reason': 'wider route frames show the same wreck and platform'}]
        apply_review(self.reward, self.row, 'report.json')
        self.assertEqual([], self.reward['matchingReview']['exclusionEvidence'])
        self.assertEqual('old scene mismatch', self.reward['routeFollowup']['before']['matchingReview']['exclusionEvidence'][0]['reason'])
        self.assertEqual(self.row['exclusionReversals'], self.reward['matchingReview']['exclusionReversals'])

    def test_new_other_candidate_guide_is_not_support(self):
        self.row['evidence'].append('guide-8-999-1.png')
        apply_review(self.reward, self.row, 'report.json')
        self.assertNotIn('guide-8-999-1.png', [g['locator'] for g in self.reward['matchingReview']['guideEvidence']])
        self.assertEqual('diagnostic', self.reward['matchingReview']['diagnosticEvidence'][0]['role'])

    def test_new_explicitly_rejected_guide_stays_excluded(self):
        self.row.update(rejectedPointIds=['999'], evidence=['route.png', 'guide-8-999-1.png'])
        apply_review(self.reward, self.row, 'report.json')
        self.assertEqual('exclusion', self.reward['matchingReview']['exclusionEvidence'][0]['role'])

    def test_revised_unknown_withdraws_identity_but_keeps_history(self):
        apply_review(self.reward, self.row, 'report.json')
        self.row.update(decision='retain-unresolved', analysis='new contradiction')
        apply_review(self.reward, self.row, 'report.json')
        self.assertEqual('pending', self.reward['identityStatus'])
        self.assertNotIn('pointId', self.reward)
        self.assertEqual('confirm', self.reward['routeFollowup']['previousRevision']['decision'])

    def test_revision_retains_full_review_decision(self):
        self.row['rejectedPointIds'] = ['999']
        apply_review(self.reward, self.row, 'report.json')
        self.row.update(analysis='revised', rejectedPointIds=['888'])
        apply_review(self.reward, self.row, 'report.json')
        self.assertEqual(['999'], self.reward['routeFollowup']['previousRevision']['reviewRow']['rejectedPointIds'])

    def test_guide_id_prefix_is_not_actual_identity(self):
        self.reward['matchingReview']['guideEvidence'] = []
        self.row['evidence'] = ['guide-8-1234-1.png']
        with self.assertRaises(ValueError):
            apply_review(self.reward, self.row, 'report.json')


if __name__ == '__main__':
    unittest.main()
