"""Append manually reviewed groups; never treats contact preparation as review."""
import json,cv2,numpy as np
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
from localize import load_reference
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'out/detectability-v2/route-huanglong-late'
P=ROOT/'tools/MarkerDetectabilityV2/route-review-huanglong-late.json';report=json.loads(P.read_text())
groups=json.loads((ROOT/'Assets/KuroMap/states/state-8.json').read_text());pts={str(p['id']):dict(p,name=g['name']) for g in groups for p in g['location']}
ref=load_reference(dict(manifest='Assets/FeaturesDatas/KuroTilePacks/jinzhou/manifest.json',bounds=[-1,1,-2,0],scale=.5))
font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',19)
def pix(raw):return ((np.array(raw)/100-ref['pixelToGame'][:,2])/np.diag(ref['pixelToGame'][:,:2]))
def route(name,stations,title):
 pp=np.array([pix(p) for _,p in stations]);lo=np.floor(pp.min(axis=0)-55).astype(int);hi=np.ceil(pp.max(axis=0)+55).astype(int);size=hi-lo;factor=min(4,1200/max(size));im=Image.fromarray(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2RGB)).crop((*lo,*hi)).resize(tuple((size*factor).astype(int)));draw=ImageDraw.Draw(im)
 def xy(raw):return tuple((pix(raw)-lo)*factor)
 for g in groups:
  if g['id'] not in ['CS_01','CS_02']:continue
  for p in g['location']:
   x,y=xy([p['x'],p['y']])
   if 0<x<im.width and 0<y<im.height:draw.ellipse((x-5,y-5,x+5,y+5),outline='#45e9ef' if g['id']=='CS_02' else '#ffb347',width=3)
 for label,raw in stations:
  x,y=xy(raw);draw.rectangle((x-5,y-5,x+5,y+5),outline='#ff5798',width=3);draw.text((max(2,x-65),y+10),label,font=font,fill='white')
 for a,b in zip(stations,stations[1:]):
  aa=np.array(xy(a[1]));bb=np.array(xy(b[1]))
  for t in np.arange(0,1,.08):draw.line([tuple(aa+(bb-aa)*t),tuple(aa+(bb-aa)*min(t+.04,1))],fill='#ffde59',width=3)
 draw.rectangle((0,0,im.width,86),fill='#101820');draw.text((8,7),title,font=font,fill='white');draw.text((8,34),'北↑ 青圈=小型信标，橙圈=中枢',font=font,fill='white');draw.text((8,59),'虚线=阶段连结，不是精确轨迹。',font=font,fill='white');im.save(OUT/name)
def add(part,ordinal,pid,analysis,stations,title,gaps=None):
 eid=f'BV1iMTXzVE8J:p{part}:e{ordinal:03}';r=next(x for x in report['members'] if x['eventId']==eid)
 fn=f'p{part}-e{ordinal}-map.png';route(fn,stations,title)
 contacts=sorted(OUT.glob(f'p{part}-e{ordinal}-route-*.jpg'));guide=ROOT/f'out/detectability-collection/BV1iMTXzVE8J/p{part:02}/guides/guide-8-{pid}-1.png'
 if not guide.exists():guide=OUT/'guides'/guide.name
 assert guide.exists()
 r.update(reviewStatus='reviewed-route-and-guide',proposal='confirm',support=[analysis],counterEvidence=['旧泛化机制候选不符合这次连续路线及已核对攻略场景；不可沿用。'],gaps=gaps or ['路线图为场景阶段连结，未把时间抽样包装为逐帧测量轨迹。'],candidates=[dict(stateId='8',pointId=pid,status='proposed-confirmed')],evidencePaths=[(OUT/fn).relative_to(ROOT).as_posix(),*[p.relative_to(ROOT).as_posix() for p in contacts],guide.relative_to(ROOT).as_posix()],detectability='undetectable')
add(7,2,'1251345370043998208','45今州城界信标转西北上建筑平台，54–60第一轮封锁青箱在柱右侧，屋外樱花和六角金砖墙；攻略1251345370043998208图正是相同柱/箱/围栏/樱花/墙。69–84爬屋顶、87返回90原柱后再开第二轮，111顶端紫箱与第一箱空间和颜色均不同；115地图屋顶箭头锚为已确认第二箱，因此第一箱不能共享1287107914627497984。', [('信标',[-29429.84,99416.05]),('第一轮青箱',[-32035.42,98464.54]),('第二轮屋顶',[-33200,96700])],'P7 e2 两轮分开：平台青箱 / 屋顶紫箱')
add(7,3,'1273389507195453440','118地图实际选中屏庭山小型信标1253813533833744384；126–147沿东向花坡，穿两块巨石的夹缝，150–156落到沙滩音障仪旁，159开青箱。攻略中沙滩坡度、左右两巨岩夹缝、岩脚层石及仪器后箱位置完全对应；其原始点[10000,119100]在该山正东海岸，旧1253173215523540992不符。', [('屏庭山信标',[-12526.59,125009.84]),('东侧花坡/石缝',[3000,120500]),('沙滩柱/青箱',[10000,119100])],'P7 e3 屏庭山东行 → 石缝 → 沙滩',gaps=['中间石缝地图位置仅示意，非模板测量；118传送及156后战斗剪辑明确断开。'])
add(7,11,'1275219716522786816','385选中龙望郊西北信标，现成全图变换下圆心426,240为raw[-100022,93481]，与信标[-100109,93382]一致；391–403西行沿路经过巨大骨架，406离路、409–412翻石，415仪器在紫花草坪。攻略紫花草坪、仪器旁浅崖层、对面立石/黑岩及通向河岸的边沿与视频一致，候选[-116100,89900]位于信标西侧，且下一事件直接从这里西北下河。', [('龙望郊信标',[-100109.22,93382.25]),('e11音障仪',[-116100,89900]),('e12河边起点',[-119900,86100])],'P7 e11 西行骨架旁 → 翻石 → 守护柱')
add(7,12,'1287101966419103744','421从前一仪器箱西北下跳到河边，427蓝球靠巨岩下岸，433–457变身沿河向西穿蓝晶枯树林，460达成后上南岸463–466领奖。攻略1蓝球在巨岩拱口及岸边，攻略2白草蓝晶、黑枯树/瀑布/河湾对应终点；468地图玩家raw[-140554,84028]为终点，原始点[-119900,86100]明确标挑战起点，不能用终点最近邻替代。', [('e11',[-116100,89900]),('河豚起点',[-119900,86100]),('沿河西行',[-130000,85000]),('468终点锚',[-140554,84028])],'P7 e12 河豚西行：起点与领奖终点分开',gaps=['430变身黑屏是界面切换；沿河中点是示意；终点使用468旧高内点锚，不拿它冒充起点。'])
add(7,13,'1276662462130065408','470返回同一龙望郊西北信标（圆心配准raw[-100045,93447]），476–506西南连续爬过四层山脊，509到峰顶射击柱，512蓝平台木箱和爆炸物，515辅助机射击后521领奖。攻略峰顶柱后的层叠山脊、远处今州高楼/城桥、左方尖塔和右方大骨架与509场景一致，原始点[-105300,107700]位于这条西南上山路线。', [('信标',[-100109.22,93382.25]),('西南山尖',[-105300,107700])],'P7 e13 同信标 → 西南连续爬山 → 射击')
add(7,15,'1280653990351872000','566实际选择庄内小型信标（圆心变换raw[-124395,116490]与点[-124390,116480]一致）；572–584向西南绕庄内木屋平台、穿垂藤和木栏，587–596沿坡下到水边高崖脚下音障仪，605紫箱。攻略仪器倚斜坡、背后竖直灰壁、白花草坡和右侧树/水岸的布局对应，原始点[-134400,134900]正是庄南偏西河谷。', [('庄内信标',[-124390,116480]),('庄南绕屋',[-128000,123000]),('下坡音障仪',[-134400,134900])],'P7 e15 庄内信标 → 绕屋下坡 → 音障仪',gaps=['庄南中点只示意绕屋方向，不声称精确路线；600战斗到605完成有剪辑，不作运动连续证明。'])
report['coverage']={'reviewedMembers':sum(r['reviewStatus']=='reviewed-route-and-guide' for r in report['members']),'unreviewedMembers':sum(r['reviewStatus']=='not-reviewed-this-pass' for r in report['members'])}
P.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(report['coverage'])
