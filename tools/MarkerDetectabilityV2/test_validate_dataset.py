import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from fractions import Fraction
import av
import numpy as np


class DatasetCliTests(unittest.TestCase):
    def fixture(self, root):
        folder=root/'out/detectability-v2/BVtest/p01';folder.mkdir(parents=True)
        # WAV has no explicit stream start timestamp. Use real timestamped A/V,
        # so the passing fixture exercises, rather than bypasses, alignment.
        audio=folder/'media.mkv'
        with av.open(str(audio),'w') as container:
            video=container.add_stream('ffv1',rate=25)
            video.width=32;video.height=32;video.pix_fmt='yuv420p'
            sound=container.add_stream('pcm_s16le',rate=16000)
            sound.layout='mono'
            for n in range(25):
                frame=av.VideoFrame.from_ndarray(np.zeros((32,32,3),np.uint8),format='rgb24')
                frame.pts=n;frame.time_base=Fraction(1,25)
                for packet in video.encode(frame):container.mux(packet)
            for packet in video.encode():container.mux(packet)
            frame=av.AudioFrame.from_ndarray(np.zeros((1,16000),np.int16),format='s16',layout='mono')
            frame.sample_rate=16000;frame.pts=0;frame.time_base=Fraction(1,16000)
            for packet in sound.encode(frame):container.mux(packet)
            for packet in sound.encode():container.mux(packet)
        timeline_dir=root/'tools/MarkerDetectabilityV2/timelines';timeline_dir.mkdir(parents=True)
        evidence=folder/'frame.jpg';evidence.write_bytes(b'evidence-existence-fixture')
        rel=lambda p:p.relative_to(root).as_posix()
        segment={'start':0.2,'end':0.4,'text':'Investigate the glow.'}
        files={folder.parent.parent/'manifest.json':[{'bvid':'BVtest','part':1,'duration':1,'videoPath':rel(audio)}],
               folder/'media.json':{'video':{'path':rel(audio),'duration':1},'audio':{'path':rel(audio),'duration':1}},
               folder/'transcript.raw.json':{'segments':[segment]},
               timeline_dir/'BVtest-p01.json':{'bvid':'BVtest','part':1,'duration':1,'reviewStatus':'reviewed','audioReviewStatus':'reviewed','progressBarReviewStatus':'reviewed',
                   'events':[{'eventId':'BVtest:p1:e001','ordinal':1,'start':0.1,'end':0.8,'rewards':[{'member':1}],
                              'transcriptRefs':[segment],'evidenceFrames':[rel(evidence)]}],
                   'reviewedEvidence':[rel(evidence)],'nonEventIntervals':[[0,0.1,'intro'],[0.8,1,'outro']]}}
        for path,data in files.items():path.write_text(json.dumps(data),encoding='utf-8')
        return files,timeline_dir/'BVtest-p01.json',folder

    def run_cli(self,root):
        script=Path(__file__).with_name('validate_dataset.py')
        self.assertTrue(script.exists(),'dataset validator CLI required')
        result=subprocess.run([sys.executable,str(script),'--root',str(root),'--expected-parts','1'],capture_output=True,text=True,encoding='utf-8')
        return result,json.loads(result.stdout)

    def test_complete_fixture_and_real_media_pass_without_modifying_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);files,_,_=self.fixture(root)
            before={p:p.read_bytes() for p in files}
            result,report=self.run_cli(root)
            self.assertEqual(0,result.returncode,result.stderr)
            self.assertTrue(report['valid']);self.assertTrue(report['canStartMatching'])
            self.assertFalse(report['canStartOverview'])
            self.assertEqual(before,{p:p.read_bytes() for p in files})

    def test_reports_missing_evidence_changed_asr_and_media_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);files,path,folder=self.fixture(root)
            t=files[path];t['events'][0]['transcriptRefs'][0]=dict(t['events'][0]['transcriptRefs'][0],text='rewritten')
            t['events'][0]['evidenceFrames']=['missing.jpg'];path.write_text(json.dumps(t),encoding='utf-8')
            m=files[folder/'media.json'];m['audio']['duration']=5
            (folder/'media.json').write_text(json.dumps(m),encoding='utf-8')
            result,report=self.run_cli(root)
            self.assertEqual(1,result.returncode)
            codes={e['code'] for e in report['errors']}
            self.assertTrue({'missing-evidence','asr-reference-mismatch','media-metadata-duration'}<=codes)
            self.assertFalse(report['canStartMatching'])

    def test_missing_manifest_part_and_unsafe_path_are_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);files,path,folder=self.fixture(root)
            t=files[path];t['reviewedEvidence']=['../outside.jpg'];path.write_text(json.dumps(t),encoding='utf-8')
            mp=folder.parent.parent/'manifest.json';manifest=files[mp];manifest.append({'bvid':'BVmissing','part':2})
            mp.write_text(json.dumps(manifest),encoding='utf-8')
            result,report=self.run_cli(root)
            self.assertEqual(1,result.returncode)
            codes={e['code'] for e in report['errors']}
            self.assertTrue({'manifest-count','missing-timeline','unsafe-path'}<=codes)
