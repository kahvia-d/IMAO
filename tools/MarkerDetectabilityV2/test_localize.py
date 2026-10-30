"""Synthetic geometric positive/negative controls, coordinate anchors and cut logic."""
import math
import json
import hashlib
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import cv2
import numpy as np
import localize as loc


class LocalizeTests(unittest.TestCase):
    def test_surface_loader_never_substitutes_surface_for_layered_tile(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); pack=root/'pack';(pack/'tiles').mkdir(parents=True)
            file=pack/'tiles/8_0_0.png';loc.save_image(file,np.full((32,32,3),90,np.uint8))
            cache=root/'map-regions/tiles/v/8';cache.mkdir(parents=True)
            (cache/'8_0_0.png').write_bytes(file.read_bytes())
            m=dict(packId='test',scene='World',resourceVersion='v',
                   source=dict(state=8,tileSize=32,virtualMapSize=850),
                   tiles=[dict(x=0,y=0,file='tiles/8_0_0.png',sha256=hashlib.sha256(file.read_bytes()).hexdigest()),
                          dict(x=0,y=0,file='tiles/layered_L1_F-1_8_0_0.png',sha256='not-the-surface-digest')])
            (pack/'manifest.json').write_text(json.dumps(m),encoding='utf-8')
            with patch.object(loc,'ROOT',root):
                ref=loc.load_reference(dict(manifest='pack/manifest.json',bounds=[0,0,0,0],scale=1))
            self.assertEqual(ref['metadata']['layer'],'surface')
            self.assertTrue((ref['image']==90).all())

    def test_tile_anchors_and_resizing(self):
        # Tile (3, 0), local pixel (512, 256): game=(2125,212.5).
        matrix=loc.tile_transform([3,5,-2,0],.5)
        np.testing.assert_allclose(loc.project(matrix,[256,128]),[2125,212.5])
        # Top-left of tile(-2, 4), not a zero-Y-origin special case.
        np.testing.assert_allclose(loc.project(loc.tile_transform([-2,2,-1,4],.25),[0,0]),[-2550,-3400])
        np.testing.assert_allclose(loc.project(loc.tile_transform([-2,2,-1,4],.25),[256,256]),[-1700,-2550])

    def test_ransac_rotation_outliers_and_spread(self):
        rng=np.random.default_rng(8)
        src=rng.uniform(40,240,(90,2)).astype(np.float32)
        angle=math.radians(23);s=1.6
        a=np.array([[s*math.cos(angle),-s*math.sin(angle),350],
                    [s*math.sin(angle),s*math.cos(angle),230]])
        dst=loc.project(a,src)+rng.normal(0,.15,src.shape)
        dst[:25]=rng.uniform(0,1000,(25,2))
        result,fit,_=loc.fit_geometry(src,dst,[140,140],110)
        self.assertTrue(result['acceptedGeometry'],result)
        np.testing.assert_allclose(loc.project(fit,[140,140]),loc.project(a,[140,140]),atol=.3)
        self.assertAlmostEqual(result['rotationDegrees'],23,delta=.2)
        self.assertGreaterEqual(result['inliers'],60)
        clustered=src*.02
        bad,_,_=loc.fit_geometry(clustered,loc.project(a,clustered),[140,140],110)
        self.assertIn('inliers-spatially-concentrated',bad['rejectionReasons'])
        self.assertFalse(bad['acceptedGeometry'])

    @staticmethod
    def texture(seed):
        rng=np.random.default_rng(seed)
        im=np.full((700,700,3),45,np.uint8)
        for _ in range(1900):
            x,y=rng.integers(0,700,2);r=int(rng.integers(1,7));c=int(rng.integers(75,200))
            cv2.circle(im,(int(x),int(y)),r,(c,c,c),-1)
        return im

    def test_end_to_end_sift_positive_and_wrong_scene(self):
        reference=self.texture(100)
        theta=math.radians(12)
        matrix=np.array([[1.15*math.cos(theta),-1.15*math.sin(theta),190],
                         [1.15*math.sin(theta),1.15*math.cos(theta),145]])
        frame=cv2.warpAffine(reference,cv2.invertAffineTransform(matrix),(300,300))
        # UI arrow + edge text are deliberately absent from the map reference.
        cv2.fillConvexPoly(frame,np.array([[143,142],[160,150],[143,158]],np.int32),(0,255,255))
        cv2.putText(frame,'N',(145,45),cv2.FONT_HERSHEY_SIMPLEX,.6,(255,255,255),2)
        roi,mask=loc.roi_mask(frame,dict(center=[150,150],radius=110,player=[150,150],excludeRects=[[135,35,35,25]]))
        self.assertEqual(mask[150,150],0);self.assertEqual(mask[42,145],0)
        def ref(im):return dict(image=im,valid=np.full(im.shape[:2],255,np.uint8),
                               pixelToGame=np.array([[1,0,0],[0,1,0]]),metadata=dict(scene='test',state=1,layer='surface'))
        result,drawing=loc.match_reference(frame,roi,mask,ref(reference))
        self.assertTrue(result['acceptedGeometry'],result)
        np.testing.assert_allclose(result['gamePosition'],loc.project(matrix,[150,150]),atol=1)
        self.assertIsNotNone(drawing)
        wrong,_=loc.match_reference(frame,roi,mask,ref(self.texture(200)))
        self.assertFalse(wrong['acceptedGeometry'],wrong)
        blank,_=loc.match_reference(np.zeros_like(frame),roi,mask,ref(reference))
        self.assertIn('insufficient-descriptors',blank['rejectionReasons'])

    def test_arrow_requires_unique_candidate_and_masks_it(self):
        frame=np.full((250,250,3),40,np.uint8)
        config=dict(center=[125,125],radius=100)
        with self.assertRaisesRegex(ValueError,'arrow ambiguous'):
            loc.roi_mask(frame,config)
        cv2.fillConvexPoly(frame,np.array([[120,117],[137,125],[120,133]],np.int32),(0,255,255))
        roi,mask=loc.roi_mask(frame,config)
        self.assertLess(np.linalg.norm(np.array(roi['player'])-[125,125]),5)
        self.assertEqual(mask[125,125],0)
        with self.assertRaisesRegex(ValueError,'outside frame'):
            loc.roi_mask(frame,dict(center=[10,10],radius=100,player=[10,10]))

    def test_continuity_and_explicit_teleport_cut(self):
        result=dict(gamePosition=[0,0],reference=dict(scene='World',state=8,layer='surface'))
        previous=dict(result=result,time=10,segmentId='shot1')
        jump=dict(gamePosition=[1000,1000],reference=result['reference'])
        frame=dict(time=11,segmentId='shot1')
        self.assertFalse(loc.continuity(previous,jump,frame)['accepted'])
        self.assertEqual(loc.continuity(previous,jump,dict(frame,cutBefore=True))['status'],'reset')
        self.assertEqual(loc.continuity(previous,jump,dict(frame,segmentId='shot2'))['status'],'reset')
        self.assertEqual(loc.continuity(previous,jump,dict(frame,time=30))['reason'],'sampling-gap')
        changed=dict(result,reference=dict(scene='Tethys',state=900,layer='surface'))
        self.assertFalse(loc.continuity(previous,changed,frame)['accepted'])

    def test_closed_gate_refuses_before_loading_frames(self):
        with patch.object(loc,'gate',return_value=False),patch.object(loc,'image') as load:
            with self.assertRaisesRegex(ValueError,'gate-closed'):
                loc.run(dict(frames=[dict(path='never-read.jpg')]))
            load.assert_not_called()


if __name__=='__main__':unittest.main()
