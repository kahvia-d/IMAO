"""Offline builder/path regression checks and JS event/media state smoke test."""
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from build_review import build, ROOT


class ReviewTests(unittest.TestCase):
    def test_reward_matching_schema(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def put(path, value):
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(json.dumps(value), encoding='utf-8')
            proof = dict(source='guide', locator='proof/map.png', reason='position', role='support')
            excluded = dict(source='guide', locator='proof/excluded.png', reason='different member', role='exclusion')
            (root / 'proof').mkdir()
            (root / 'proof/map.png').touch()
            (root / 'proof/excluded.png').touch()
            rewards = [dict(member=i, matchingReview=dict(analysis=f'member-{i}',
                candidates=[dict(stateId=f'000{i}', pointId='900719925474099312345')],
                positionEvidence=[proof, dict(excluded, role='support')],
                videoEvidence=[dict(source='test:p1', locator='00:01–00:03', reason='source clip', role='support')],
                guideEvidence=[dict(proof, locator='proof/map.png#index')], exclusionEvidence=[excluded], missingEvidence=['need guide'],
                attributeReview=dict(status='pending', analysis='attribute only', evidence=[]))) for i in range(1, 4)]
            put('out/detectability-v2/manifest.json', [dict(bvid='test', part=1)])
            put('tools/MarkerDetectabilityV2/timelines/test-p01.json', dict(bvid='test', part=1, duration=10,
                events=[dict(eventId='test:p1:e001', ordinal=1, start=0, end=10, rewards=rewards)]))
            report = build(root)
            self.assertEqual(report['checkedAssets'], 3)
            self.assertEqual(report['pathIssues'], [])
            page = Path(report['output'])
            self.assertIn('map.png#index', page.read_text(encoding='utf-8'))
            harness = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(process.argv[2],'utf8');
const payload=html.match(/<script id="review-data" type="application\/json">([\s\S]*?)<\/script>/)[1];
const source=html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const elements={};const document={getElementById(id){return elements[id]??= {textContent:id==='review-data'?payload:'',innerHTML:'',addEventListener(){},querySelectorAll(){return []},pause(){},load(){},removeAttribute(){}}}};
const context=vm.createContext({document,assert,console});vm.runInContext(source,context);
vm.runInContext(`
const first=rewardReview(event().rewards[0]),third=rewardReview(event().rewards[2]);
assert(first.includes('0001'));assert(!first.includes('0003'));assert(third.includes('0003'));assert(!third.includes('0001'));
assert(third.includes('900719925474099312345'));
assert.equal(time(null),'未知');assert.equal(time(undefined),'未知');assert.equal(time(''),'未知');assert.equal(time(0),'0:00.0');
assert(third.includes('位置证据'));assert(third.includes('视频证据'));assert(third.includes('攻略证据'));
assert(third.includes('为什么不确定'));assert(third.includes('视频里是什么样的'));assert(third.includes('攻略里是什么样的'));assert(third.includes('<details><summary>详细证据'));
assert(third.includes('缺失证明'));assert(third.includes('属性审阅'));assert(third.includes('need guide'));
const support=third.split('data-review-section="support"')[1].split('data-review-section="exclusion"')[0];
assert(!support.includes('excluded.png'));assert(third.includes('excluded.png'));
assert(third.includes('00:01–00:03'));assert(!third.includes('资源不可用'));
assert(evidence([{source:'x',locator:'<script>bad()</script>',reason:'x',role:'support'}]).includes('&lt;script&gt;'));
console.log('reward schema separation and string IDs OK');
`,context);
'''
            script = root / 'schema.cjs'
            script.write_text(harness, encoding='utf-8')
            result = subprocess.run(['node', str(script), str(page)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_source_paths_are_renderable_and_missing_locator_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            def put(path,data):
                p=root/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(data),encoding='utf-8')
            (root/'proof').mkdir();(root/'proof/guide.png').touch()
            put('out/detectability-v2/manifest.json',[dict(bvid='test',part=1)])
            reward=dict(member=1,matchingReview=dict(guideEvidence=[dict(source='proof/guide.png',locator='image1',reason='guide',role='support')],videoEvidence=[dict(source='test:p1',locator='proof/missing.jpg',reason='source',role='support')]))
            put('tools/MarkerDetectabilityV2/timelines/test-p01.json',dict(bvid='test',part=1,duration=10,events=[dict(eventId='test:p1:e001',ordinal=1,start=0,end=10,rewards=[reward])]))
            report=build(root);html=Path(report['output']).read_text('utf-8')
            payload=json.loads(html.split('<script id="review-data" type="application/json">')[1].split('</script>')[0])
            self.assertIn('proof/guide.png',payload['assets'])
            self.assertTrue(any(i['path']=='proof/missing.jpg' for i in report['pathIssues']))

    def test_safe_paths_and_gates(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def put(path, data):
                p = root / path
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(json.dumps(data), encoding='utf-8')
            put('out/detectability-v2/manifest.json', [dict(bvid='test', part=1, videoPath='media/video with space.mp4')])
            (root / 'media').mkdir()
            (root / 'media/video with space.mp4').touch()
            timeline = dict(bvid='test', part=1, duration=10, reviewStatus='reviewed',
                            audioReviewStatus='reviewed', progressBarReviewStatus='reviewed',
                            events=[], nonEventIntervals=[[0, 10, '</script><script>bad()</script>']])
            put('tools/MarkerDetectabilityV2/timelines/test-p01.json', timeline)
            report = build(root)
            html = Path(report['output']).read_text(encoding='utf-8')
            self.assertTrue(report['matchingGate'])
            self.assertIn('video%20with%20space.mp4', html)
            self.assertNotIn('</script><script>bad()', html)
            timeline['nonEventIntervals'] = [[0, 5, 'gap']]
            timeline['events'] = [dict(eventId='e', ordinal=1, start=0, end=1,
                                       evidenceFrames=['../outside.jpg', 'missing.jpg'], rewards=[])]
            put('tools/MarkerDetectabilityV2/timelines/test-p01.json', timeline)
            report = build(root)
            self.assertFalse(report['matchingGate'])
            self.assertFalse(report['overviewGate'])
            self.assertEqual({i['error'] for i in report['pathIssues']}, {'missing file', 'path outside repository'})

    def test_javascript_all_events_and_media_sync(self):
        page = ROOT / 'out/detectability-v2/review/index.html'
        harness = r"""
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(process.argv[2],'utf8');
const payload=html.match(/<script id="review-data" type="application\/json">([\s\S]*?)<\/script>/)[1];
const source=html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
class Element {
 constructor(){this.innerHTML='';this.textContent='';this.readyState=1;this.currentTime=0;this.playbackRate=1;this.paused=true;this.checked=true;this.listeners={};this.dataset={};this.classList={toggle(){}}}
 querySelectorAll(selector){const matches=selector==='button'?[...this.innerHTML.matchAll(/<button\b/g)]:[...this.innerHTML.matchAll(/data-seek="([^"]+)"/g)];return matches.map(m=>{const e=new Element();e.dataset.seek=m[1];return e})}
 addEventListener(name,fn){this.listeners[name]=fn}
 removeAttribute(name){delete this[name]}
 load(){} pause(){this.paused=true} play(){this.paused=false;return Promise.resolve()}
}
const elements={};const document={getElementById(id){if(!elements[id])elements[id]=new Element();return elements[id]}};
document.getElementById('review-data').textContent=payload;
const context=vm.createContext({document,console,assert});
vm.runInContext(source,context);
vm.runInContext(`
assert.equal(identityQueue.length,D.results.unresolved.length);
let count=0;
for(let p=0;p<D.parts.length;p++){
 choosePart(p);assert.equal(current(),D.parts[p]);
 for(let e=0;e<(current().timeline?.events?.length||0);e++){
  chooseEvent(e);count++;assert.equal(video.currentTime,event().start);
  assert($('eventMeta').textContent.includes(event().eventId));
  assert($('eventContent').innerHTML.includes('来源画面'));
  assert($('eventContent').innerHTML.includes('匹配支持'));
  assert($('eventContent').innerHTML.includes('排除证据'));
 }
}
choosePart(D.parts.findIndex(p=>p.audio&&url(p.audio)));
video.currentTime=23;audio.currentTime=0;sync();assert.equal(audio.currentTime,23);
video.playbackRate=1.5;sync();assert.equal(audio.playbackRate,1.5);
seek(7);assert.equal(video.currentTime,7);assert.equal(audio.currentTime,7);
video.paused=false;audio.paused=false;stop();assert(video.paused&&audio.paused);
video.readyState=0;seek(13);assert.equal(seekPending,13);video.readyState=1;video.listeners.loadedmetadata();assert.equal(video.currentTime,13);
console.log('JS runtime: '+D.parts.length+' parts, '+count+' events, seeking/rate/pause/deferred metadata OK');
`,context);
"""
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / 'review-smoke.cjs'
            script.write_text(harness, encoding='utf-8')
            result = subprocess.run(['node', str(script), str(page)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
