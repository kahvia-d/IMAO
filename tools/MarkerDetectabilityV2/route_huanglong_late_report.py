"""Render reviewed route segments and record actual review coverage."""
import json
from pathlib import Path
import cv2,numpy as np
from PIL import Image,ImageDraw,ImageFont
from localize import load_reference,project
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'out/detectability-v2/route-huanglong-late'
read=lambda p:json.loads((ROOT/p).read_text('utf-8-sig'))
ref=load_reference(dict(manifest='Assets/FeaturesDatas/KuroTilePacks/jinzhou/manifest.json',bounds=[1,2,-4,-3],scale=.5))
rows=read('out/detectability-v2/route-huanglong-late/p12-terrain.json')
points=read('Assets/KuroMap/states/state-8.json')
def pixel(raw):return ((np.array(raw)/100-ref['pixelToGame'][:,2])/np.diag(ref['pixelToGame'][:,:2])).tolist()
im=Image.fromarray(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2RGB)).resize((1536,1536));draw=ImageDraw.Draw(im)
font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',20)
def xy(raw):return tuple(v*1.5 for v in pixel(raw))
facilities=[]
for g in points:
 if g['id'] not in ['CS_01','CS_02']:continue
 for p in g['location']:
  x,y=xy([p['x'],p['y']])
  if not(0<x<1536 and 0<y<1536):continue
  c='#45e9ef' if g['id']=='CS_02' else '#ffb347';draw.ellipse((x-7,y-7,x+7,y+7),outline=c,width=3)
  label=('S' if g['id']=='CS_02' else 'N')+str(len(facilities)+1);draw.text((x+8,y-10),label,fill=c,font=font)
  facilities.append(dict(label=label,stateId='8',pointId=str(p['id']),type=g['name'],rawPosition=[p['x'],p['y']],observedUsed=p['id']=='1255956793434292224'))
segments=[]
for lo,hi in [(390,420),(440,505)]:
 rr=[r for r in rows if lo<=r['time']<=hi];coords=[xy(r['peaks'][0]['rawPosition']) for r in rr]
 draw.line(coords,fill='#ffde59',width=4)
 for r,c in zip(rr,coords):
  draw.ellipse((c[0]-3,c[1]-3,c[0]+3,c[1]+3),fill='#ffde59')
  if r['time']%10==0:draw.text((c[0]+6,c[1]),str(r['time']),font=font,fill='#ffffff')
 segments.append(dict(start=lo,end=hi,method='visually-reviewed-north-up-terrain-correlation',samples=[dict(time=r['time'],**r['peaks'][0]) for r in rr]))
ids=['1288594077072769024','1251555676983664640','1287380389842522112'];targets=[]
for i,pid in enumerate(ids):
 p=next(p for g in points for p in g['location'] if p['id']==pid);x,y=xy([p['x'],p['y']]);draw.rectangle((x-8,y-8,x+8,y+8),outline='#ff5798',width=3);draw.text((x+10,y-28),f'e{i+6}',font=font,fill='#ff5798');targets.append(p)
draw.rectangle((10,10,1520,125),fill='#101820');draw.text((24,18),'P12 385–510秒：稷廷遗址信标 → 碎石箱 → 拼图屋 → 战斗挑战',font=font,fill='white')
draw.text((24,49),'青圈S=小型信标；橙圈N=中枢信标；粉框=候选箱；黄线=已目检地形样本连接（非逐帧轨迹）',font=font,fill='white')
draw.text((24,80),'425–435秒错误峰已剔除，不连接缺口；380传送断开前一事件；510后传送另起段。北↑',font=font,fill='white')
im.save(OUT/'p12-route-map.png')
detail=Image.fromarray(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2RGB)).crop((390,170,650,330)).resize((1300,800));dd=ImageDraw.Draw(detail)
def detailxy(raw):
 x,y=pixel(raw);return ((x-390)*5,(y-170)*5)
for ss in segments:
 cc=[detailxy(r['rawPosition']) for r in ss['samples']];dd.line(cc,fill='#ffde59',width=5)
 for r,c in zip(ss['samples'],cc):
  if r['time'] in [390,400,410,420,440,455,475,485,495,505]:dd.text(c,str(r['time']),font=font,fill='white')
for i,p in enumerate(targets):
 x,y=detailxy([p['x'],p['y']]);dd.rectangle((x-9,y-9,x+9,y+9),outline='#ff5798',width=3);dd.text((x-70,y+12),f'e{i+6}',font=font,fill='#ff5798')
x,y=detailxy([72018.32,300797.41]);dd.ellipse((x-8,y-8,x+8,y+8),outline='#45e9ef',width=4);dd.text((x-80,y-35),'起点小型信标',font=font,fill='#45e9ef')
detail.save(OUT/'p12-route-detail.png')
early=Image.fromarray(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2RGB)).crop((30,300,260,560)).resize((920,1040));ed=ImageDraw.Draw(early)
def exy(raw):
 x,y=pixel(raw);return ((x-30)*4,(y-300)*4)
stations=[('10 信标',[31981.69,310564.81]),('26 崖边',[22744,319580]),('30 下坡',[20088,320908]),('42 挑战起点',[16300,325100]),('87 终点地图箭头',[18786.72,335613.33])]
for label,p in stations:
 x,y=exy(p);ed.ellipse((x-5,y-5,x+5,y+5),fill='#ffde59');ed.text((x+9,y),label,font=font,fill='white')
for a,b in zip(stations,stations[1:]):
 aa=np.array(exy(a[1]));bb=np.array(exy(b[1]))
 for t in np.arange(0,1,.09):ed.line([tuple(aa+(bb-aa)*t),tuple(aa+(bb-aa)*min(t+.045,1))],fill='#ffde59',width=3)
ed.rectangle((0,0,920,100),fill='#101820');ed.text((10,10),'P12 e1 北↑：信标→过水到小岛→收集光点绕岛→南端领奖',font=font,fill='white');ed.text((10,42),'虚线仅为已审阅阶段的连结，不表示逐帧测量路径。',font=font,fill='white');ed.text((10,70),'10/26/30模板锚；42攻略起点；87地图箭头锚。',font=font,fill='white');early.save(OUT/'p12-e1-route-map.png')
report=dict(schemaVersion=1,bvid='BV1iMTXzVE8J',parts=list(range(7,13)),reviewStatus='partial',method='north-up-terrain-template-plus-continuous-scene-and-transcript-review',facilities=facilities,otherTransportFacilities='本次审阅385–510只使用1255956793434292224小型信标；地图副本符号作为地标，不认定实际传送使用。',routeSegments=segments,discontinuities=[dict(start=380,end=385,reason='teleport'),dict(start=425,end=435,reason='terrain peak contaminated by dungeon icon; no absolute positions accepted')],members=[])
support=['从稷廷遗址小型信标向东绕岩，400空地左拐、405贴山、410北入小道，415/420地形位置raw约85166/85000,298994/298828；碎石破开后紫花与岩缝对应攻略。','e6后翻岩北行，440–465围绕同一菱形房屋拼图，470/475进屋位置raw86660,295508/295342；四块拼图/烧荆棘/晶核及室内桌椅斜板对应攻略。','e7出门向东沿小路，485东移后490右转向南到战斗柱，495–505在同一草坪；510开图181内点且目检玩家426,241，raw92641.7,301558.2，距候选3.61游戏单位；攻略挑战柱后方破墙、林木、地形一致。']
guides=['out/detectability-collection/BV1iMTXzVE8J/p12/guides/guide-8-1288594077072769024-1.png','out/detectability-collection/BV1iMTXzVE8J/p12/additional-review/guides/guide-8-1251555676983664640-1.png','out/detectability-collection/BV1iMTXzVE8J/p12/guides/guide-8-1287380389842522112-1.png']
for part in range(7,13):
 timeline=read(f'tools/MarkerDetectabilityV2/timelines/BV1iMTXzVE8J-p{part:02}.json')
 for e in timeline['events']:
  for r in e['rewards']:
   if r['identityStatus']=='confirmed':continue
   row=dict(eventId=e['eventId'],member=r['member'],reviewStatus='not-reviewed-this-pass',proposal='retain-unresolved',support=[],counterEvidence=[],gaps=['本轮尚未连续跟跑审阅；不把候选提取或旧审阅算完成。'],candidates=[])
   if part==12 and e['ordinal'] in (6,7,8):
    i=e['ordinal']-6
    row.update(reviewStatus='reviewed-route-and-guide',proposal='confirm',support=[support[i]],counterEvidence=['旧文字候选位置与本次路线绝对位置不符，不能沿用。'],gaps=['425–435模板受副本图标污染，明确保留断段；前后地形及房屋本体可分别定位。'] if i==1 else [],candidates=[dict(stateId='8',pointId=ids[i],status='proposed-confirmed')],evidencePaths=['out/detectability-v2/route-huanglong-late/p12-route-map.png',f'out/detectability-v2/route-huanglong-late/p12-chain-{0 if i==0 else 9 if i==1 else 18}.jpg',f'out/detectability-v2/route-huanglong-late/p12-terrain-{0 if i==0 else 9 if i==1 else 18}.jpg',guides[i]],transcriptRefs=e['transcriptRefs'],detectability='undetectable')
   if part==12 and e['ordinal']==1:
    row.update(reviewStatus='reviewed-route-and-guide',proposal='confirm',support=['10秒小地图道路与海岸模板得分.60，raw32207,310615，对应小型信标1255956793388154880；西南经过帐篷，26/30秒raw22744,319580及20088,320908沿崖下到水边；34–42过水到独立小岛北端挑战柱，柱后倒木、草坡层崖与针叶树分布对应攻略图1；46–82收集光点沿岛缘到南端，紫花坡和紫箱对应攻略图2；87地图玩家位于该岛南端raw18786.7,335613.3。点位标在挑战起点16300,325100，不是领奖终点，不能仅依终点最近邻排除。'],counterEvidence=['旧候选1285624536032034816/1258931888181538816不在此次信标—小岛路线。'],gaps=['34–82多数模板最高峰为错误海岸，未采用；路线中间只作场景阶段连结，精确绕岛轨迹未测量。'],candidates=[dict(stateId='8',pointId='1282456677512798208',status='proposed-confirmed')],evidencePaths=['out/detectability-v2/route-huanglong-late/p12-e1-route-map.png','out/detectability-v2/route-huanglong-late/p12-e1-chain-0.jpg','out/detectability-v2/route-huanglong-late/p12-e1-chain-9.jpg','out/detectability-v2/route-huanglong-late/p12-e1-chain-18.jpg','out/detectability-v2/review-root/BV1iMTXzVE8J-p12/map-anchors.json','out/detectability-collection/BV1iMTXzVE8J/p12/guides/guide-8-1282456677512798208-1.png','out/detectability-collection/BV1iMTXzVE8J/p12/guides/guide-8-1282456677512798208-2.png'],transcriptRefs=e['transcriptRefs'],detectability='undetectable')
   report['members'].append(row)
report['coverage']={'reviewedMembers':sum(r['reviewStatus']=='reviewed-route-and-guide' for r in report['members']),'unreviewedMembers':sum(r['reviewStatus']=='not-reviewed-this-pass' for r in report['members'])}
third=read('out/detectability-v2/route-huanglong-late/p12-e3-terrain.json')
thirdmap=Image.fromarray(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2RGB)).crop((440,365,620,590)).resize((900,1125));td=ImageDraw.Draw(thirdmap)
def txy(raw):
 x,y=pixel(raw);return ((x-440)*5,(y-365)*5)
accepted=[r for r in third if 201<=r['time']<=261]
td.line([txy(r['peaks'][0]['rawPosition']) for r in accepted],fill='#ffde59',width=4)
for r in accepted:
 if r['time'] in [201,213,225,231,243,255,261]:td.text(txy(r['peaks'][0]['rawPosition']),str(r['time']),font=font,fill='white')
for label,raw,color in [('小型信标',[85890.02,319209.06],'#45e9ef'),('挑战起点 1287379692742459392',[92300,339200],'#ff5798')]:
 x,y=txy(raw);td.ellipse((x-7,y-7,x+7,y+7),outline=color,width=3);td.text((x-180,y+15),label,font=font,fill=color)
td.rectangle((0,0,900,90),fill='#101820');td.text((15,10),'P12 e3 北↑：信标南下→爬上高台→向东至挑战→蓝平台',font=font,fill='white');td.text((15,43),'黄色=201–261已目检地形样本；弱峰/传送不连线。',font=font,fill='white');thirdmap.save(OUT/'p12-e3-route-map.png')
row=next(r for r in report['members'] if r['eventId']=='BV1iMTXzVE8J:p12:e003')
row.update(reviewStatus='reviewed-route-and-guide',proposal='confirm',support=['171–177明确传送断链；183落地后南行，189小地图峰对应信标南侧raw86992,323730；201–219南下绕怪、在213攀高台，地形相关.53/.68/.69/.67与长条海崖完全同形；225向东到挑战柱，231启动位置raw92305,339170，与1287379692742459392起点几乎一致；237–261蓝平台先向海再绕回崖侧，相关.43–.58，连续地形及视频场景符合；攻略柱旁蓝花草坡、近处立壁松树与两岸峡谷相符。'],counterEvidence=['旧两条泛化跑酷候选与稷廷南端长条海崖位置不符。'],gaps=['267–285模板相关仅.36–.43，本次不连接至终点，不据这些低峰做绝对身份判断；起点地形、长段路线和攻略已独立吻合。'],candidates=[dict(stateId='8',pointId='1287379692742459392',status='proposed-confirmed')],evidencePaths=['out/detectability-v2/route-huanglong-late/p12-e3-route-map.png','out/detectability-v2/route-huanglong-late/p12-e3-chain-0.jpg','out/detectability-v2/route-huanglong-late/p12-e3-chain-9.jpg','out/detectability-v2/route-huanglong-late/p12-e3-chain-18.jpg','out/detectability-v2/route-huanglong-late/p12-e3-terrain-0.jpg','out/detectability-v2/route-huanglong-late/p12-e3-terrain-9.jpg','out/detectability-collection/BV1iMTXzVE8J/p12/guides/guide-8-1287379692742459392-1.png'],detectability='undetectable')
report['coverage']={'reviewedMembers':5,'unreviewedMembers':len(report['members'])-5}
ref7=load_reference(dict(manifest='Assets/FeaturesDatas/KuroTilePacks/jinzhou/manifest.json',bounds=[-1,1,-2,0],scale=.5))
def p7(raw):return ((np.array(raw)/100-ref7['pixelToGame'][:,2])/np.diag(ref7['pixelToGame'][:,:2])).tolist()
map7=Image.fromarray(cv2.cvtColor(ref7['image'],cv2.COLOR_BGR2RGB)).crop((420,570,650,840)).resize((920,1080));d7=ImageDraw.Draw(map7)
def c7(raw):
 x,y=p7(raw);return ((x-420)*4,(y-570)*4)
for g in points:
 if g['id'] not in ['CS_01','CS_02']:continue
 for p in g['location']:
  x,y=c7([p['x'],p['y']])
  if 0<x<920 and 0<y<1080:d7.ellipse((x-7,y-7,x+7,y+7),outline='#45e9ef' if g['id']=='CS_02' else '#ffb347',width=3);d7.text((x+8,y),g['name'],font=font,fill='white')
station7=[('216 欲燃之森副本',[-86615,128286]),('e5 射击挑战',[-80800,120200]),('254 西北行',[-83505.86,118369.14]),('258 北侧坡',[-85664.06,115878.91]),('e6 跳入地坑',[-87300,112300])]
for label,p in station7:
 x,y=c7(p);d7.rectangle((x-7,y-7,x+7,y+7),outline='#ff5798',width=3);d7.text((x+10,y),label,font=font,fill='white')
for a,b in zip(station7,station7[1:]):
 aa=np.array(c7(a[1]));bb=np.array(c7(b[1]))
 for t in np.arange(0,1,.09):d7.line([tuple(aa+(bb-aa)*t),tuple(aa+(bb-aa)*min(t+.045,1))],fill='#ffde59',width=3)
d7.rectangle((0,0,920,110),fill='#101820');d7.text((10,10),'P7 e5/e6：欲燃之森副本 → 东北射击 → 西北地坑',font=font,fill='white');d7.text((10,42),'北↑；青圈小型信标，橙圈中枢；粉框阶段地点。',font=font,fill='white');d7.text((10,75),'虚线是有口播/场景支持的阶段连结，非精确测量轨迹。',font=font,fill='white');map7.save(OUT/'p7-e5e6-route-map.png')
report['facilities'].append(dict(label='P7实际传送副本',stateId='8',pointId='1253802978924298240',type='欲燃之森',rawPosition=[-86615,128286],observedUsed=True,evidence='out/detectability-v2/route-huanglong-late/p7-216.jpg'))
report['otherTransportFacilities']='P7 216秒实际选择欲燃之森副本；P12审阅段使用小型信标；其他地图副本图标不等于实际使用。'
for ordinal,pid,explain in [(5,'1282070799312306176','216地图选中欲燃之森，配准raw[-86637,128374]与副本点[-86615,128286]吻合；222–238越过树干向东北，固定北向小地图副本图标逐步向左下移动；238射击挑战位于凸石左侧。攻略射击柱旁耸立灰岩、下坡层崖和远处今州建筑对应；238/242/250弱模板峰指向raw约-81000,120200，但仅作辅助不当独立确认。'),(6,'1287383082180341760','250离开射击柱向西北，254/258同形山脊地形峰raw[-83506,118369]/[-85664,115879]；262跳入地坑，266/270蓝色花丛拱洞、左侧枯树与远处今州建筑和攻略两图一致，270模板raw[-86826,112393]接近候选[-87300,112300]；连续下行把射击点和洞中战斗挑战相连。')]:
 rr=next(r for r in report['members'] if r['eventId']==f'BV1iMTXzVE8J:p7:e{ordinal:03}')
 rr.update(reviewStatus='reviewed-route-and-guide',proposal='confirm',support=[explain],counterEvidence=['旧候选与本次欲燃之森东北—西北连续路线及攻略场景不符。'],gaps=['此段小地图右下固定雷达干扰地形相关；多帧最高峰错误已拒绝，未用其拼精确路线；图中只给阶段连结。'],candidates=[dict(stateId='8',pointId=pid,status='proposed-confirmed')],evidencePaths=['out/detectability-v2/route-huanglong-late/p7-e5e6-route-map.png','out/detectability-v2/route-huanglong-late/p7-216.jpg','out/detectability-v2/route-huanglong-late/p7-216-facility-fit.json','out/detectability-v2/route-huanglong-late/p7-e5e6-chain-0.jpg','out/detectability-v2/route-huanglong-late/p7-e5e6-chain-9.jpg',f'out/detectability-collection/BV1iMTXzVE8J/p07/guides/guide-8-{pid}-1.png'],detectability='undetectable')
report['coverage']={'reviewedMembers':7,'unreviewedMembers':len(report['members'])-7}
rr=next(r for r in report['members'] if r['eventId']=='BV1iMTXzVE8J:p7:e008')
rr.update(reviewStatus='reviewed-route-and-guide',proposal='confirm',support=['接续e6洞口270向东出洞，274右转爬山、278–286翻坡到巡尉任务山顶，294转东滑翔到左侧哨塔；314开启跑酷后穿光点，322落到营地棚顶领白箱。攻略图1是同一临河金属哨塔（今州入城桥及对岸建筑一致），攻略图2棚顶箱旁猫窝/三花猫、身后铁门、右侧哨塔及瀑布与322画面对应。点1282071617490993152标在起点哨塔[-72400,108200]，终点棚顶与已确认e9巡卫小队长[-71000,111800]上下对应。'],counterEvidence=['旧候选1287104752935976960不在这段出洞—山坡—今州临河哨塔的连贯路线。'],gaps=['爬山滑翔段仅按固定北向地图、连续场景与两端已确认地点画阶段虚线，未给逐帧测量轨迹。'],candidates=[dict(stateId='8',pointId='1282071617490993152',status='proposed-confirmed')],evidencePaths=['out/detectability-v2/route-huanglong-late/p7-e8-route-map.png','out/detectability-v2/route-huanglong-late/p7-e8-chain-0.jpg','out/detectability-v2/route-huanglong-late/p7-e8-chain-9.jpg','out/detectability-collection/BV1iMTXzVE8J/p07/guides/guide-8-1282071617490993152-1.png','out/detectability-collection/BV1iMTXzVE8J/p07/guides/guide-8-1282071617490993152-2.png'],detectability='undetectable')
e8map=map7.copy();e8d=ImageDraw.Draw(e8map)
st=[('e6洞口',[-87300,112300]),('314 起点哨塔',[-72400,108200]),('322 棚顶/325巡卫',[-71000,111800])]
for label,p in st:
 x,y=c7(p);e8d.rectangle((x-7,y-7,x+7,y+7),outline='#7cff6b',width=3);e8d.text((x-150,y-30),label,font=font,fill='#7cff6b')
for a,b in zip(st,st[1:]):
 aa=np.array(c7(a[1]));bb=np.array(c7(b[1]))
 for t in np.arange(0,1,.09):e8d.line([tuple(aa+(bb-aa)*t),tuple(aa+(bb-aa)*min(t+.045,1))],fill='#7cff6b',width=3)
e8d.rectangle((0,0,920,110),fill='#101820');e8d.text((10,10),'P7 e8 北↑：出洞爬山→滑翔至哨塔→跑酷到棚顶',font=font,fill='white');e8d.text((10,43),'绿色虚线=阶段连结；不表示精确路径。黄色为e5–e6。',font=font,fill='white');e8d.text((10,76),'棚顶位于已确认e9巡卫上方；起点/终点以攻略两图核验。',font=font,fill='white');e8map.save(OUT/'p7-e8-route-map.png')
report['coverage']={'reviewedMembers':8,'unreviewedMembers':len(report['members'])-8}
(ROOT/'tools/MarkerDetectabilityV2/route-review-huanglong-late.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(report['coverage'])
