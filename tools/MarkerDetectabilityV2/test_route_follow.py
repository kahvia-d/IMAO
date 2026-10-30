import unittest
import cv2
import numpy as np
from route_follow import displacement, integrate, terrain_candidates


class RouteFollowTests(unittest.TestCase):
    def test_north_up_terrain_template_reports_source_center(self):
        rng=np.random.default_rng(47)
        atlas=cv2.GaussianBlur(rng.integers(0,256,(400,400),dtype=np.uint8),(5,5),0)
        query=atlas[130:230,180:280].copy()
        mask=np.full_like(query,255);mask[:10]=0;mask[-10:]=0;mask[:,:10]=0;mask[:,-10:]=0
        best=terrain_candidates(query,mask,atlas,count=2)[0]
        np.testing.assert_allclose(best['centerPixel'],[230,180],atol=1)
        self.assertGreater(best['score'],.9)
    def test_terrain_motion_is_opposite_player_motion(self):
        rng = np.random.default_rng(22)
        a = rng.integers(0, 256, (220, 220), dtype=np.uint8)
        a = cv2.GaussianBlur(a, (3, 3), 0)
        b = cv2.warpAffine(a, np.float32([[1, 0, -13], [0, 1, 7]]), (220, 220))
        result = displacement(a, b, np.full_like(a, 255), np.full_like(a, 255))
        self.assertTrue(result['accepted'])
        np.testing.assert_allclose(result['playerDelta'], [13, -7], atol=.6)

    def test_unrelated_frame_is_gap_not_motion(self):
        rng = np.random.default_rng(24)
        a = rng.integers(0, 256, (220, 220), dtype=np.uint8)
        b = rng.integers(0, 256, (220, 220), dtype=np.uint8)
        self.assertFalse(displacement(a,b,np.full_like(a,255),np.full_like(b,255))['accepted'])

    def test_explicit_cut_and_failed_link_break_accumulation(self):
        steps = [{'time':0}, {'time':1,'accepted':True,'playerDelta':[2,3]},
                 {'time':2,'accepted':True,'playerDelta':[90,90],'cutBefore':True},
                 {'time':3,'accepted':False}, {'time':4,'accepted':True,'playerDelta':[1,2]}]
        tracks=integrate(steps)
        self.assertEqual([r['segment'] for r in tracks],[0,0,1,2,2])
        self.assertEqual(tracks[-1]['relativePixel'],[1.,2.])

    def test_zoom_is_not_accumulated_as_walking(self):
        rng=np.random.default_rng(25);a=cv2.GaussianBlur(rng.integers(0,256,(220,220),dtype=np.uint8),(3,3),0)
        b=cv2.warpAffine(a,cv2.getRotationMatrix2D((110,110),0,1.13),(220,220))
        self.assertFalse(displacement(a,b,np.full_like(a,255),np.full_like(b,255))['accepted'])
