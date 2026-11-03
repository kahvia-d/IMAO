'use strict';

// 六种界面风格定义：保留 01-05 原有方案，在末尾补充 06 寰宇星图高定方案供切换对比
const themes = [
  ['pixel', '精密像素工具', 'PIXEL PRECISION', '硬边像素、台阶细节、紧凑工具布局。'],
  ['tactical', '地图图例 × 战术终端', 'TACTICAL ATLAS', '坐标网格、等高线、仪表定位标记。'],
  ['street', '黑白 × 机能街头', 'UTILITY MONO', '粗黑边框、荧光标记、强对比排版。'],
  ['signal', '声波频谱 × 游戏 HUD', 'RESONANCE HUD', '共振圆环、发光细线、切角操作按钮。'],
  ['glass', '透明设备 × 极简猫咪', 'CLEAR DEVICE', '半透明外壳、柔和材质、小型猫咪标识。'],
  ['aetheria', '寰宇星图 × 黑海岸高定', 'AETHERIA ATLAS', '曜金陶瓷与声波星图仪、可交互等高线航点、实时频谱示波器与暗夜/白昼双生高定色盘。']
];

// 主导航页面定义（含 06 高定主题的英文副标题代号）
const pages = [
  ['home', '首页', 'M3 10l9-7 9 7v11h-6v-7H9v7H3z', 'OVERVIEW'],
  ['routes', '路线', 'M5 4h14v16H5zM8 8h8M8 12h5M8 16h8', 'ROUTES'],
  ['filter', '点位筛选', 'M3 4h18l-7 9v7l-4-2v-5z', 'POINT FILTER'],
  ['settings', '设置', 'M3 6h18M3 12h18M3 18h18M8 3v6m8 0v6m-9 0v6', 'CONFIG'],
  ['diagnostics', '诊断', 'M2 12h5l3-8 5 16 3-8h4', 'TELEMETRY'],
  ['help', '帮助', 'M3 4h7l2 3 2-3h7v16h-7l-2 2-2-2H3zM12 7v15', 'MANUAL']
];

// 全局共享状态：切换任何主题均完整保留当前页面、探索状态、筛选结果与开关设置
const state = {
  theme: 'aetheria',
  aetheriaLight: false,
  page: 'home',
  running: false,
  source: '本地客户端',
  settingsTab: '显示',
  category: '全部类型',
  query: '',
  selectedOnly: false,
  selected: new Set(['声匣', '普通宝箱', '灯笼果']),
  toggles: {
    mini: true,
    map: true,
    completed: false,
    minibar: true,
    mapbar: false,
    ball: true,
    replan: true,
    gamepad: true,
    gpu: false,
    autoUpdate: true,
    autoSync: false,
    synced: false,
    noDiff: false
  },
  values: {},
  route: 0,
  collection: '全部合集',
  batch: false,
  checked: new Set(),
  capture: false,
  syncPreview: false,
  // 06 寰宇星图专属交互状态（图层开关与当前检视航点）
  mapLayers: { contour: true, route: true, radar: true },
  activeWaypoint: 3
};

// 示例路线数据（含 06 航线蓝图节点坐标）
const routeData = [
  { name: '今州 · 宝箱收集', group: '日常探索', kind: '自动路线', count: 24, done: 8, coords: '32°14\'N · 118°49\'E', path: 'M32 110 C84 92, 124 46, 180 72 S258 34, 328 42', nodes: [[32, 110], [104, 72], [180, 72], [254, 48], [328, 42]] },
  { name: '中曲台地 · 采集', group: '日常探索', kind: '手绘路线', count: 18, done: 0, coords: '31°52\'N · 119°04\'E', path: 'M28 96 L88 64 L146 86 L212 38 L276 58 L326 28', nodes: [[28, 96], [88, 64], [146, 86], [212, 38], [276, 58], [326, 28]] },
  { name: '黑海岸 · 声匣', group: '收集物', kind: '自动路线', count: 12, done: 3, coords: '29°08\'N · 122°31\'E', path: 'M30 44 Q110 120, 182 64 T324 52', nodes: [[30, 44], [112, 84], [182, 64], [256, 40], [324, 52]] }
];

// 今州星图 6 个可交互示例航点数据
const waypointData = [
  { id: '01', name: '声匣 · 今州城界北塔', x: 118, y: 184, elv: '+62m', status: '已完成', done: true },
  { id: '02', name: '普通宝箱 · 云陵谷口', x: 204, y: 132, elv: '+88m', status: '已完成', done: true },
  { id: '03', name: '灯笼果 · 屏庭山麓', x: 292, y: 154, elv: '+114m', status: '已完成', done: true },
  { id: '04', name: '普通宝箱 09 · 祈池村遗迹', x: 392, y: 92, elv: '+146m', status: '下一目标', next: true },
  { id: '05', name: '高级宝箱 · 荒石高地', x: 488, y: 134, elv: '+182m', status: '待探索' },
  { id: '06', name: '声匣 · 归墟港市边缘', x: 576, y: 76, elv: '+95m', status: '待探索' }
];

// 点位筛选数据
const pointData = [
  ['声匣', '收集物', '◇', 128],
  ['普通宝箱', '宝箱', '▣', 64],
  ['高级宝箱', '宝箱', '▣', 26],
  ['灯笼果', '采集物', '♧', 42],
  ['金铃子', '采集物', '♧', 56],
  ['云芝', '采集物', '♧', 32],
  ['巡哨机傀', '敌人', '⌖', 18],
  ['哀声鸷', '敌人', '⌖', 6],
  ['飞猎手', '敌人', '⌖', 25],
  ['简易宝箱', '宝箱', '▣', 48],
  ['中级宝箱', '宝箱', '▣', 34],
  ['霄凤兰', '采集物', '♧', 22]
];

const $ = s => document.querySelector(s);
const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

// 基础 UI 构造辅助函数
function button(text, action, primary = false) {
  return `<button data-action="${action}"${primary ? ' class="primary"' : ''}>${text}</button>`;
}
function toggle(key, label) {
  return `<button class="toggle" role="switch" aria-label="${label}" aria-checked="${state.toggles[key]}" data-toggle="${key}"></button>`;
}
function setting(key, label, desc = '') {
  return `<div class="setting-row"><div>${label}${desc ? `<p>${desc}</p>` : ''}</div>${toggle(key, label)}</div>`;
}
function select(label, options, key) {
  return `<div class="setting-row"><label for="${key}">${label}</label><select id="${key}" data-value="${key}">${options.map(x => `<option${state.values[key] === x ? ' selected' : ''}>${x}</option>`).join('')}</select></div>`;
}
function number(label, key, value, min, max, unit) {
  return `<div class="setting-row"><label for="${key}">${label}<p>${unit}</p></label><input id="${key}" data-value="${key}" type="number" min="${min}" max="${max}" value="${esc(state.values[key] ?? value)}"></div>`;
}
function panel(title, content, meta = '', cls = '') {
  return `<section class="panel ${cls}"><div class="panel-head"><h2>${title}</h2>${meta ? `<span class="subtle">${meta}</span>` : ''}</div>${content}</section>`;
}
function heading(title, sub, code) {
  if (state.theme === 'aetheria') {
    const idx = String(pages.findIndex(p => p[0] === state.page) + 1).padStart(2, '0');
    return `<div class="ae-page-head"><div><div class="ae-eyebrow">SOLARIS-3 // ${code}</div><h1>${title}</h1><p>${sub}</p></div><div class="ae-coord-stamp"><b>SEC // ${idx} · ${code}</b><span>32°14'N · 118°49'E</span></div></div>`;
  }
  return `<div class="heading"><div><div class="eyebrow">IMAO / ${code}</div><h1>${title}</h1><p>${sub}</p></div><span class="serial">EXPLORATION TOOL / ${String(pages.findIndex(p => p[0] === state.page) + 1).padStart(2, '0')}</span></div>`;
}

// ============================================================================
// 01-05 原始视图模板（保持隔壁 AI 助手原有结构与表现，确保公平对比）
// ============================================================================
function mapSvg() {
  return `<svg class="map" viewBox="0 0 540 210" role="img" aria-label="今州路线示意图，非游戏实际地图"><defs><pattern id="grid" width="30" height="30" patternUnits="userSpaceOnUse"><path d="M30 0H0V30" class="grid" fill="none"/></pattern></defs><rect width="540" height="210" fill="url(#grid)"/><path class="land" d="M0 27L74 14 116 42 183 20 233 60 270 45 335 73 402 34 467 55 540 24V210H0Z"/><path class="contour" d="M-20 75Q60 0 141 74T296 92T450 66T570 100M-20 91Q60 16 141 90T296 108T450 82T570 116M-20 107Q60 32 141 106T296 124T450 98T570 132M-20 123Q60 48 141 122T296 140T450 114T570 148M-20 139Q60 64 141 138T296 156T450 130T570 164"/><path class="river" d="M242 -20Q165 60 239 90T308 153T252 230"/><path class="route-line" d="M111 143L160 106 218 118 287 72 353 105 412 69"/>${[[111, 143], [160, 106], [218, 118], [287, 72], [353, 105], [412, 69]].map(([x, y], i) => `<circle class="point" cx="${x}" cy="${y}" r="5"/><text x="${x + 10}" y="${y - 8}">0${i + 1}</text>`).join('')}<path class="player" d="M108 134l-7 20 11-5 10 4z"/><text x="26" y="30">今州 · 示意</text><text x="472" y="31">N ↑</text><path d="M455 187h57m-57-4v8m57-8v8" class="contour"/><text x="455" y="177">路线示意</text></svg>`;
}
function home() {
  return heading('首页', '连接游戏，查看本次探索状态。', 'OVERVIEW') + `<div class="overview">${panel('游戏连接', `<div class="connection"><div class="connection-icon">⌖</div><div><strong>${state.running ? '正在探索' : '准备开始探索'}</strong><p>${esc(state.source)} · 鸣潮 <span class="badge">模拟</span></p></div></div><div class="actions">${button(state.running ? '■ 停止探索' : '▶ 开始探索', 'start', true)}${button('选择游戏窗口', 'window')}<span class="tiny">F9 快捷切换</span></div><div class="source-row"><span>核心状态 · 演示就绪</span><button data-action="reconnect">重新连接</button></div>`, 'CONNECTION')}${panel('当前路线', `<h3 class="route-title">今州 · 宝箱收集</h3><p class="tiny">下一目标 · 普通宝箱 09</p><div class="route-stats"><div><strong>08 <span class="tiny">/ 24</span></strong><small>已完成点位</small></div><div><strong>16</strong><small>剩余目标</small></div></div><progress max="24" value="8" aria-label="路线完成进度"></progress><div class="route-foot"><span class="tiny">导航暂停 · 示例状态</span><button data-nav="routes">查看路线 →</button></div>`, 'CURRENT ROUTE')}</div><div class="lower">${panel('地图与点位', mapSvg() + `<div class="map-legend"><span>▲ 位置示意</span><span>◇ 路线目标</span><span>┄ 访问顺序</span></div>`, 'ILLUSTRATIVE MAP', 'map-panel')}${panel('常用设置', `<div class="quick"><div class="quick-row"><div><span>小地图点位</span><p>在小地图显示已选类型</p></div>${toggle('mini', '小地图点位')}</div><div class="quick-row"><div><span>大地图点位</span><p>在游戏大地图显示点位</p></div>${toggle('map', '大地图点位')}</div><div class="quick-row"><div><span>显示已完成点位</span><p>不影响已有完成记录</p></div>${toggle('completed', '显示已完成点位')}</div></div><div class="summary-line"><span>已选 <b>${state.selected.size}</b> 类</span><span>记录本 <b>默认</b></span><a href="#filter">调整筛选 →</a></div>`, 'QUICK SETTINGS')}</div>`;
}
function routes() {
  return heading('路线', '整理已保存的路线与合集。选点和导航在游戏内进行。', 'ROUTES') + `<div class="toolbar"><label for="collection">合集</label><select id="collection">${['全部合集', '日常探索', '收集物'].map(x => `<option${state.collection === x ? ' selected' : ''}>${x}</option>`).join('')}</select>${button('新建合集', 'collection-new')}${button('管理合集', 'collection-manage')}<span style="flex:1"></span>${button('导入路线', 'route-import')}${button('导出合集', 'route-export')}${button(state.batch ? '退出批量' : '批量管理', 'batch')}</div>${panel('路线列表', `<table class="route-table"><thead><tr><th>${state.batch ? '选择' : '顺序'}</th><th>路线名称</th><th>目标</th><th>类型</th><th>完成</th></tr></thead><tbody>${routeData.map((r, i) => state.collection !== '全部合集' && r.group !== state.collection ? '' : `<tr data-route="${i}" class="${state.route === i ? 'selected' : ''}"><td>${state.batch ? `<input type="checkbox" data-check="${i}" aria-label="选择${esc(r.name)}"${state.checked.has(i) ? ' checked' : ''}>` : `<span class="order">0${i + 1}</span>`}</td><td><button class="route-select" data-pick-route="${i}" aria-pressed="${state.route === i}">${esc(r.name)}</button><small>${r.group}${i === 0 ? ' · 当前路线' : ''}</small></td><td>${r.count} 个</td><td>${r.kind}</td><td>${r.done} / ${r.count}</td></tr>`).join('')}</tbody></table><div class="route-detail"><div><h3>${esc(routeData[state.route]?.name ?? '暂无路线')}</h3><p>删除路线不会删除点位完成记录。</p></div><div class="actions">${state.batch ? button('全选', 'batch-all') + button('移动所选', 'route-move') + button('导出所选', 'route-export') : button('排序说明', 'route-sort')}${button(state.batch ? '删除所选' : '删除路线', 'route-delete')}</div></div>`, `${routeData.filter(r => state.collection === '全部合集' || r.group === state.collection).length} ROUTES`)}<div class="notice">游戏内大地图 → 地图工具台 → 路径自动规划。预览只表示平面访问顺序，不代表实际可通行路径。</div><details><summary>游戏内路线操作与手绘功能</summary><p>选点、框选、套索、指定起点、预览、开始指引、暂停/继续、跳过、重新规划、刷怪采集、手绘、自动轮换均保留在游戏内。</p><div class="actions" style="margin-top:14px">${button('查看完整操作', 'overlay-features')}</div></details>`;
}
function filter() {
  return heading('点位筛选', '选择想在地图上显示的类型，完成记录不受影响。', 'POINT FILTER') + `<div class="toolbar"><input id="search" type="search" placeholder="搜索点位或材料" aria-label="搜索点位或材料" value="${esc(state.query)}"><select id="category" aria-label="点位分类">${['全部类型', '收集物', '宝箱', '采集物', '敌人', '角色养成', '武器养成'].map(x => `<option${state.category === x ? ' selected' : ''}>${x}</option>`).join('')}</select><button class="chip" id="selected-only" aria-pressed="${state.selectedOnly}">仅看已选</button></div>${['角色养成', '武器养成'].includes(state.category) ? `<div class="notice" style="margin:0 0 18px">当前演示采用${state.category}材料组合示例，真实预设来自点位目录。</div>` : ''}<div class="filter-summary"><span id="selection-summary"></span><div class="actions">${button('全选当前结果', 'filter-all')}${button('取消选择当前结果', 'filter-clear')}</div></div><div class="items" id="items"></div><div class="notice">演示修改即时生效，仅保留在当前页面会话中。正式客户端会保存筛选；筛选不会改变点位完成状态。</div><details><summary>筛选同步与提示</summary><p>正常时显示简短保存状态。目录异常、同步失败时显示原因与“重试同步”入口。</p>${button('查看异常状态示例', 'filter-error')}</details>`;
}

// 设置页 5 个分组内容
const settingsContent = {
  '显示': () => panel('地图点位', setting('mini', '小地图点位') + setting('map', '大地图点位') + setting('completed', '显示已完成点位', '关闭仅隐藏，不撤销已完成记录。'), 'OVERLAY') + panel('状态提示', setting('minibar', '小地图状态条') + setting('mapbar', '大地图状态条') + setting('ball', '小地图状态球'), 'INDICATORS'),
  '操作': () => panel('键盘快捷键', select('开始 / 停止探索', ['F9', 'F8', 'F7'], 'keyStart') + select('完成点位', ['F6', 'F5', 'F4'], 'keyDone') + select('打开 / 关闭攻略', ['F8', 'F7', 'F6'], 'keyGuide') + `<div class="actions" style="margin-top:15px">${button('全部快捷键', 'shortcuts')}${button('保存快捷键', 'save-shortcuts')}</div><p class="section-note">示例按键用于视觉展示。实际保留键与冲突检查需要生产实现。</p>`, 'HOTKEYS') + panel('手柄', setting('gamepad', '启用手柄') + select('设备', ['自动选择', 'Xbox / XInput（模拟）', 'PlayStation（模拟）'], 'pad') + select('按键提示', ['Xbox', 'PlayStation'], 'layout') + `<div class="actions" style="margin-top:15px">${button('刷新设备', 'refresh-pad')}${button('手柄速查', 'gamepad-help')}</div>`, 'CONTROLLER') + panel('触发范围', number('完成点位', 'rangeDone', 15, 5, 120, '小地图像素') + number('查看攻略', 'rangeGuide', 15, 5, 120, '小地图像素') + number('刷怪采集自动标记', 'rangeFarm', 30, 5, 120, '小地图像素'), 'RADIUS') + panel('路线与标记', setting('replan', '自动重排路线', '偏离时调整剩余目标顺序。') + number('图标合并重叠阈值', 'merge', 50, 1, 100, '% · 自由点始终单独显示'), 'ROUTING'),
  '记录与同步': () => panel('本地记录本', `<div class="setting-row"><span>当前记录本</span><b>默认</b></div><div class="setting-row"><span>库街区账号</span><span class="tiny">未绑定 · 演示</span></div><div class="actions" style="margin-top:17px">${button('新建记录本', 'profile-new')}${button('修改记录本', 'profile-edit')}${button('删除记录本', 'profile-delete')}</div><details><summary>记录本工具</summary><div class="actions" style="margin-top:12px">${button('导出检查报告', 'profile-report')}${button('旧数据恢复', 'legacy')}${button('打开目录', 'local-folder')}</div></details>`, 'NOTEBOOK') + panel('库街区进度同步', setting('autoSync', '自动同步', '正式客户端：标记后推送，每 10 分钟补齐。') + select('地图区域', ['全部区域', '瑝珑', '黑海岸', '黎那汐塔'], 'syncArea') + setting('synced', '显示已同步数量') + setting('noDiff', '显示无差异区域') + `<div class="actions" style="margin-top:17px">${button('预览同步', 'sync-preview', true)}<button data-action="sync-apply"${state.syncPreview ? '' : ' disabled'}>应用同步</button></div><details><summary>连接与维护</summary><div class="actions" style="margin-top:12px">${button('修复浏览器桥接', 'bridge')}${button('断开连接', 'disconnect')}${button('同步数据目录', 'local-folder')}</div></details>`, 'CLOUD SYNC'),
  '地图与更新': () => panel('版本与更新', `<div class="setting-row"><span>程序版本</span><b>2026.11.1.108</b></div><div class="setting-row"><span>地图资源</span><span class="tiny">版本状态示例</span></div>${setting('autoUpdate', '启动后检查更新')}<div class="actions" style="margin-top:16px">${button('检查更新', 'update-check', true)}${button('导入离线包', 'offline')}</div><details><summary>下载、回退与异常恢复</summary><div class="actions" style="margin-top:12px">${button('下载程序', 'program-download')}${button('下载地图资源', 'resources-download')}${button('退出并更新', 'restart-update')}${button('取消下载', 'cancel-download')}${button('回退程序', 'rollback-program')}${button('回退地图资源', 'rollback-resource')}${button('修复更新状态', 'repair-update')}</div></details>`, 'VERSION') + panel('下载源', select('下载来源', ['GitHub', 'Mirror酱'], 'downloadSource') + `<div class="setting-row"><span>连通性</span><span class="tiny">未检测</span></div><div class="actions" style="margin-top:16px">${button('检测连通性', 'connectivity')}${button('管理 CDK', 'cdk')}</div><p class="section-note">此草图不接收真实凭据，也不发起网络下载。</p>`, 'MIRROR') + panel('区域地图资源', `<div class="setting-row"><span>瑝珑</span><span class="badge">已启用 · 示例</span></div><div class="setting-row"><span>黑海岸</span><span class="badge">已启用 · 示例</span></div><div class="setting-row"><span>黎那汐塔</span><span class="tiny">未下载 · 示例</span></div><div class="actions" style="margin-top:15px">${button('管理区域', 'regions')}</div><p class="section-note">启用/停用控制加载；删除资源会释放本地空间。</p>`, 'REGION PACKS', 'full'),
  '高级': () => panel('捕获与刷新', select('画面捕获方式', ['BitBlt', 'Windows Graphics Capture'], 'captureMethod') + select('叠加层呈现', ['分层窗口', 'DirectComposition'], 'present') + number('小地图刷新间隔', 'miniRate', 50, 16, 1000, '毫秒') + number('大地图刷新间隔', 'mapRate', 100, 16, 1000, '毫秒') + `<p class="section-note">正式客户端需停止叠加后切换捕获。这里的数值仅为演示，不代表推荐设置。</p>`, 'CAPTURE') + panel('性能与兼容性', setting('gpu', '坐标识别 GPU 加速', '正式客户端需完全退出重开，不支持时回退 CPU。') + `<div class="actions" style="margin-top:16px">${button('应用窗口兼容设置', 'compat')}${button('恢复图形默认设置', 'compat-reset')}</div><details><summary>数据目录</summary><div class="actions" style="margin-top:12px">${button('点位进度目录', 'local-folder')}${button('路线文件目录', 'local-folder')}</div></details><div class="notice">DirectComposition 为可选呈现方式，此草图不宣称其性能更好。</div>`, 'PERFORMANCE')
};

function settings() {
  return heading('设置', '按用途分组，常用选项直接调整。', 'PREFERENCES') + `<div class="tabs">${Object.keys(settingsContent).map(t => `<button data-settings-tab="${t}" aria-pressed="${t === state.settingsTab}">${t}</button>`).join('')}</div><div class="settings-grid">${settingsContent[state.settingsTab]()}</div>`;
}
function diagnostics() {
  return heading('诊断', '查看运行状态，采集问题线索。', 'DIAGNOSTICS') + `<div class="small-grid"><div class="metric"><span>核心连接</span><b>未接入</b></div><div class="metric"><span>坐标识别后端</span><b>待报告</b></div><div class="metric"><span>运行环境</span><b>HTML 演示</b></div></div><div class="settings-grid" style="margin-top:18px">${panel('短时完整采集', `<p class="tiny">正式客户端最多记录 3 分钟，可提前停止。</p><div class="actions" style="margin-top:18px">${button(state.capture ? '提前停止' : '开始 3 分钟采集', 'capture', true)}${button('打开采集文件夹', 'local-folder')}</div><div class="notice">${state.capture ? '采集界面演示中；没有录制画面或写入文件。' : '此处仅模拟采集状态，不记录游戏画面。'}</div>`, 'CAPTURE RECORDER')}${panel('日志与报告', `<div class="log">[UI] 草图已载入\n[CORE] 未连接真实核心\n[OCR] 无实际后端报告\n[STATUS] 演示状态</div><div class="actions">${button('打开日志', 'local-folder')}${button('诊断摘要', 'diagnostic-summary')}${button('崩溃报告', 'crash')}</div>`, 'LOG STREAM')}</div><details><summary>高级诊断：性能对照与隔离</summary><p>隐藏叠加窗口、暂停呈现、每帧工作隔离开关。仅在定位问题时使用。</p><div class="actions" style="margin-top:12px">${button('查看诊断工具', 'diagnostic-tools')}</div></details>`;
}
function help() {
  return heading('帮助', '入门与操作说明，按需展开。', 'HELP') + `<div class="settings-grid">${panel('开始使用', `<div class="quick-row"><div><h3>01 连接游戏</h3><p>首页选择游戏窗口，然后开始探索。</p></div></div><div class="quick-row"><div><h3>02 筛选点位</h3><p>按类型、角色或武器选择目标。</p></div></div><div class="quick-row"><div><h3>03 游戏内规划路线</h3><p>打开大地图工具台，选点、预览并开始指引。</p></div></div><div class="actions" style="margin-top:18px">${button('手柄速查', 'gamepad-help')}${button('攻略与点位操作', 'guide-help')}</div>`, 'ONBOARDING')}${panel('更多帮助', `<details open><summary>路线、手绘与取消操作</summary><p>暂停保留路线；退出导航结束本次导航。选点不是完成点位，平面访问顺序不是通行路径。</p>${button('查看完整操作', 'overlay-features')}</details><details><summary>旧数据恢复</summary><p>扫描旧记录、未登记记录本和删除保留的副本。</p>${button('查看恢复流程', 'legacy')}</details><details><summary>关于 IMAO</summary><p>鸣潮地图探索辅助工具。程序版本 2026.11.1.108。此处是设计探索草图。</p><a href="https://github.com/kahvia-d/WWMAP-TOOLS" target="_blank" rel="noreferrer">项目源码 ↗</a></details><details><summary>本次功能整理</summary><p>44 组功能，包含桌面、游戏内工具及攻略窗口。候选分组与文案不改变真实实现。</p>${button('打开功能清单', 'inventory')}</details>`, 'KNOWLEDGE BASE')}</div>`;
}

const views = { home, routes, filter, settings, diagnostics, help };

// ============================================================================
// 06 寰宇星图高定版（AETHERIA ATLAS）专属视觉组件与高定视图
// ============================================================================

// 点位类型定制矢量纹章（替代平庸的 Unicode 文本字符）
function pointEmblemSvg(name, category) {
  if (name === '声匣' || category === '收集物') {
    return `<svg viewBox="0 0 28 28" aria-hidden="true"><polygon points="14,2 25,9 25,19 14,26 3,19 3,9"/><polygon points="14,6 21,14 14,22 7,14" stroke-dasharray="2 1.5"/><circle cx="14" cy="14" r="2.2" fill="currentColor"/></svg>`;
  }
  if (category === '宝箱') {
    return `<svg viewBox="0 0 28 28" aria-hidden="true"><rect x="3" y="8" width="22" height="15" rx="2"/><path d="M3 13h22M9 8V6a5 5 0 0 1 10 0v2"/><rect x="12" y="11" width="4" height="5" rx="1" fill="currentColor"/></svg>`;
  }
  if (category === '采集物') {
    return `<svg viewBox="0 0 28 28" aria-hidden="true"><path d="M14 25V11"/><path d="M14 17c-6 0-9-4-9-9 5 0 9 3 9 9z"/><path d="M14 14c6 0 9-4 9-9-5 0-9 3-9 9z"/><circle cx="14" cy="6" r="2" fill="currentColor"/></svg>`;
  }
  return `<svg viewBox="0 0 28 28" aria-hidden="true"><circle cx="14" cy="14" r="9"/><path d="M14 2v5M14 21v5M2 14h5M21 14h5"/><polygon points="14,10 18,14 14,18 10,14" fill="currentColor"/></svg>`;
}

// 点位稀有度配色映射
function pointAccentColor(name, category) {
  if (name === '声匣' || name === '高级宝箱' || name === '哀声鸷') return '#e5c17b';
  if (name === '中级宝箱' || category === '采集物') return '#4ce0d2';
  if (category === '敌人') return '#ff647c';
  return '#9ab8d4';
}

// 06 首页：高定共鸣星盘仪 SVG
function aeCoreAstrolabeSvg() {
  const accent = state.running ? '#4ce0d2' : '#e5c17b';
  return `<svg class="ae-dial-svg" viewBox="0 0 88 88" aria-hidden="true">
    <circle cx="44" cy="44" r="40" fill="none" stroke="rgba(229,193,123,.2)" stroke-width="1"/>
    <g class="ae-dial-ring">
      <circle cx="44" cy="44" r="35" fill="none" stroke="${accent}" stroke-width="1.4" stroke-dasharray="4 6"/>
      <path d="M44 4v6M44 78v6M4 44h6M78 44h6" stroke="${accent}" stroke-width="1.5"/>
    </g>
    <circle cx="44" cy="44" r="25" fill="rgba(229,193,123,.06)" stroke="rgba(229,193,123,.35)" stroke-width="1"/>
    <path d="M29 52V33l8 5h14l8-5v19z" fill="none" stroke="${accent}" stroke-width="1.8" stroke-linejoin="round"/>
    <circle cx="38" cy="43" r="1.6" fill="${accent}"/>
    <circle cx="50" cy="43" r="1.6" fill="${accent}"/>
    <path d="M41 48h6" stroke="${accent}" stroke-width="1.6" stroke-linecap="round"/>
  </svg>`;
}

// 06 首页：可交互今州声波等高线星图 SVG
function aeAtlasMapSvg() {
  const showContour = state.mapLayers.contour;
  const showRoute = state.mapLayers.route;
  const showRadar = state.mapLayers.radar;
  const wp = waypointData[state.activeWaypoint] || waypointData[3];
  return `<svg class="ae-atlas-svg" viewBox="0 0 680 256" role="img" aria-label="今州声波等高线星图示意，非游戏实际地图">
    <defs>
      <pattern id="aeGrid" width="34" height="34" patternUnits="userSpaceOnUse">
        <path d="M34 0H0V34" fill="none" stroke="rgba(229,193,123,.08)" stroke-width="0.7"/>
        <circle cx="34" cy="34" r="0.9" fill="rgba(229,193,123,.24)"/>
      </pattern>
      <radialGradient id="aeRadarGrad" cx="50%" cy="50%" r="50%">
        <stop offset="0%" stop-color="#4ce0d2" stop-opacity="0.32"/>
        <stop offset="100%" stop-color="#4ce0d2" stop-opacity="0"/>
      </radialGradient>
    </defs>
    <rect width="680" height="256" fill="url(#aeGrid)"/>
    <!-- 地貌基底与流域水系 -->
    <path d="M0 44 L92 22 L168 54 L254 26 L338 68 L428 36 L524 62 L612 28 L680 46 V256 H0 Z" fill="rgba(229,193,123,.04)" stroke="rgba(229,193,123,.18)" stroke-width="1"/>
    <path d="M298 -10 C248 64, 318 112, 366 168 S332 230, 308 266" fill="none" stroke="rgba(76,224,210,.16)" stroke-width="22" stroke-linecap="round"/>
    <path d="M298 -10 C248 64, 318 112, 366 168 S332 230, 308 266" fill="none" stroke="rgba(76,224,210,.45)" stroke-width="1.5" stroke-dasharray="6 4"/>
    ${showContour ? `<g stroke="rgba(229,193,123,.22)" fill="none" stroke-width="1">
      <path d="M-20 86 Q85 18, 190 88 T395 96 T610 72 T700 104"/>
      <path d="M-20 112 Q85 44, 190 114 T395 122 T610 98 T700 130" stroke-dasharray="3 3"/>
      <path d="M-20 138 Q85 70, 190 140 T395 148 T610 124 T700 156"/>
      <path d="M-20 166 Q85 98, 190 168 T395 176 T610 152 T700 184" stroke-dasharray="2 4"/>
      <path d="M-20 194 Q85 126, 190 196 T395 204 T610 180 T700 212"/>
      <text x="68" y="64" fill="rgba(229,193,123,.48)" font-size="9" font-family="Consolas">ELV +120m · 云陵谷</text>
      <text x="418" y="56" fill="rgba(229,193,123,.48)" font-size="9" font-family="Consolas">ELV +180m · 屏庭山</text>
      <text x="515" y="196" fill="rgba(229,193,123,.48)" font-size="9" font-family="Consolas">ELV +95m · 归墟港市</text>
    </g>` : ''}
    ${showRadar ? `<g>
      <circle cx="292" cy="154" r="54" fill="url(#aeRadarGrad)" stroke="rgba(76,224,210,.32)" stroke-width="1" stroke-dasharray="4 4"/>
      <circle cx="292" cy="154" r="28" fill="none" stroke="rgba(76,224,210,.25)" stroke-width="0.8"/>
      <path d="M292 154 L338 112 A62 62 0 0 1 352 158 Z" fill="rgba(76,224,210,.16)"/>
    </g>` : ''}
    ${showRoute ? `<g>
      <path d="M118 184 L204 132 L292 154 L392 92 L488 134 L576 76" fill="none" stroke="rgba(229,193,123,.25)" stroke-width="5" stroke-linecap="round"/>
      <path class="ae-route-vec" d="M118 184 L204 132 L292 154 L392 92 L488 134 L576 76" fill="none" stroke="#e5c17b" stroke-width="2.2"/>
    </g>` : ''}
    <!-- 选中航点的全图十字游标准星线 -->
    <g stroke="rgba(229,193,123,.26)" stroke-width="0.9" stroke-dasharray="3 3">
      <line x1="${wp.x}" y1="0" x2="${wp.x}" y2="256"/>
      <line x1="0" y1="${wp.y}" x2="680" y2="${wp.y}"/>
    </g>
    <!-- 6 个可点击检视的航点坐标节点 -->
    ${waypointData.map((p, idx) => {
      const isSelected = state.activeWaypoint === idx;
      const color = p.next ? '#4ce0d2' : (p.done ? '#e5c17b' : '#94a3b8');
      return `<g class="ae-wp-node" data-wp="${idx}" role="button" tabindex="0" aria-label="检视航点 ${p.id} ${p.name}">
        ${isSelected ? `<circle cx="${p.x}" cy="${p.y}" r="14" fill="none" stroke="${color}" stroke-width="1.4" stroke-dasharray="3 2"/><circle cx="${p.x}" cy="${p.y}" r="20" fill="none" stroke="${color}" stroke-opacity="0.35" stroke-width="1"/>` : ''}
        <circle class="ae-wp-core" cx="${p.x}" cy="${p.y}" r="${isSelected ? 6.5 : 5}" fill="#0b1018" stroke="${color}" stroke-width="2.2"/>
        <text x="${p.x - 8}" y="${p.y - 12}" fill="${isSelected ? '#fff' : color}" font-size="10" font-weight="700" font-family="Consolas">${p.id}</text>
      </g>`;
    }).join('')}
    <!-- 玩家当前位置信标 -->
    <g transform="translate(292,154)">
      <polygon points="0,-10 8,8 0,4 -8,8" fill="#4ce0d2" stroke="#09101a" stroke-width="1.2"/>
    </g>
    <!-- 右上角精密星盘罗盘 -->
    <g transform="translate(628,44)">
      <circle r="22" fill="rgba(9,13,20,.7)" stroke="rgba(229,193,123,.4)" stroke-width="1"/>
      <path d="M0 -18 L4 -4 L18 0 L4 4 L0 18 L-4 4 L-18 0 L-4 -4 Z" fill="none" stroke="#e5c17b" stroke-width="1"/>
      <polygon points="0,-16 3,-3 -3,-3" fill="#4ce0d2"/>
      <text x="-4" y="-24" fill="#e5c17b" font-size="9" font-family="Consolas" font-weight="700">N</text>
    </g>
    <!-- 左上角星图铭牌 -->
    <g transform="translate(20,26)">
      <text x="0" y="0" fill="#e5c17b" font-size="11" font-weight="700" letter-spacing="1">今州 · 声波等高线星图示意</text>
      <text x="0" y="15" fill="#8b97ab" font-size="9.5" font-family="Consolas">SECTOR JINZHOU // 点击航点 01-06 锁定十字游标与目标遥测</text>
    </g>
    <!-- 右下角选中航点浮动标尺 -->
    <g transform="translate(450,234)">
      <path d="M0 0h70m-70-4v8m70-8v8" stroke="#e5c17b" stroke-width="1.2"/>
      <text x="82" y="4" fill="#e5c17b" font-size="9.5" font-family="Consolas">FOCUS #${wp.id} (${wp.elv})</text>
    </g>
  </svg>`;
}

// 06 首页高定视图
function aeHome() {
  const wp = waypointData[state.activeWaypoint] || waypointData[3];
  const segments = Array.from({ length: 24 }, (_, i) => `<i class="${i < 8 ? 'done' : (i === 8 ? 'next' : '')}" title="点位 ${String(i + 1).padStart(2, '0')}"></i>`).join('');
  const selectedList = Array.from(state.selected);
  return heading('首页', '连接游戏终端，统览实时共鸣测绘与航线进度。', 'OVERVIEW') + `
  <div class="ae-home-stage">
    ${panel('游戏连接 · 终端点火台', `
      <div class="ae-command-hero">
        <div class="ae-core-dial-wrap">
          ${aeCoreAstrolabeSvg()}
          <div class="ae-core-info">
            <span class="badge" style="margin-bottom:8px;display:inline-block">${state.running ? '● 实时声波追踪中' : '◇ 待命准备就绪'}</span>
            <strong>${state.running ? '正在探索' : '准备开始探索'}</strong>
            <p>${esc(state.source)} · 鸣潮 <span class="badge">模拟</span></p>
          </div>
        </div>
        <div class="ae-launch-bar">
          <button class="primary" data-action="start">${state.running ? '■ 停止探索' : '▶ 开始探索'}<span class="ae-keycap">F9</span></button>
          ${button('选择游戏窗口', 'window')}
        </div>
        <div class="ae-telemetry-strip">
          <div class="ae-mini-stat"><span>核心状态</span><b>演示就绪</b></div>
          <div class="ae-mini-stat"><span>快捷热键</span><b>F9 快捷切换</b></div>
          <div class="ae-mini-stat"><span>链路维护</span><b><button data-action="reconnect">重新连接 ↻</button></b></div>
        </div>
      </div>
    `, 'CONNECTION // H01')}
    <section class="panel ae-map-panel">
      <div class="panel-head">
        <h2>地图与点位</h2>
        <div class="ae-map-layer-chips" aria-label="星图图层开关">
          <button class="ae-layer-chip" data-map-layer="contour" aria-pressed="${state.mapLayers.contour}">等高线</button>
          <button class="ae-layer-chip" data-map-layer="route" aria-pressed="${state.mapLayers.route}">航线矢量</button>
          <button class="ae-layer-chip" data-map-layer="radar" aria-pressed="${state.mapLayers.radar}">声波雷达</button>
        </div>
      </div>
      <div class="ae-map-stage">${aeAtlasMapSvg()}</div>
      <div class="ae-map-inspector">
        <div class="ae-wp-detail">
          <span class="ae-wp-badge">航点 #${wp.id}</span>
          <b>${esc(wp.name)}</b>
          <span class="tiny">海拔 ${wp.elv} · 状态：${wp.status}</span>
        </div>
        <div class="actions">
          <span class="tiny">▲ 位置示意 · ◇ 路线目标 · ┄ 访问顺序</span>
          <button data-action="guide-help" style="padding:5px 10px;font-size:11px">攻略与点位操作 ↗</button>
        </div>
      </div>
    </section>
  </div>
  <div class="ae-home-lower">
    ${panel('当前路线', `
      <div class="ae-chrono-row">
        <svg class="ae-ring-gauge" viewBox="0 0 78 78" aria-label="路线完成进度 8 / 24">
          <circle cx="39" cy="39" r="32" fill="none" stroke="rgba(229,193,123,.14)" stroke-width="6"/>
          <circle cx="39" cy="39" r="32" fill="none" stroke="#e5c17b" stroke-width="6" stroke-linecap="round" stroke-dasharray="67 201" transform="rotate(-90 39 39)"/>
          <text x="39" y="37" text-anchor="middle" fill="var(--ink)" font-size="15" font-weight="800" font-family="Consolas">08/24</text>
          <text x="39" y="51" text-anchor="middle" fill="var(--muted)" font-size="9" font-family="Consolas">33.3%</text>
        </svg>
        <div>
          <h3 class="route-title" style="margin:0 0 6px">今州 · 宝箱收集</h3>
          <p class="tiny">下一目标 · <b style="color:var(--accent-cyan)">普通宝箱 09</b></p>
          <p class="tiny" style="margin-top:4px">已完成 <b>08</b> 点位 · 剩余 <b>16</b> 目标</p>
        </div>
      </div>
      <div class="ae-seg-bar" aria-hidden="true">${segments}</div>
      <div class="route-foot">
        <span class="tiny">导航暂停 · 示例状态</span>
        <button data-nav="routes">查看路线 →</button>
      </div>
    `, 'CURRENT ROUTE // H03')}
    ${panel('常用设置', `
      <div class="ae-quick-list">
        <div class="ae-quick-item">
          <div class="ae-quick-icon" aria-hidden="true"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="2"/><path d="M12 4v3M12 17v3M4 12h3M17 12h3"/></svg></div>
          <div><span>小地图点位</span><p>在小地图显示已选类型</p></div>
          ${toggle('mini', '小地图点位')}
        </div>
        <div class="ae-quick-item">
          <div class="ae-quick-icon" aria-hidden="true"><svg viewBox="0 0 24 24"><path d="M3 6l6-2 6 2 6-2v14l-6 2-6-2-6 2z"/><path d="M9 4v14M15 6v14"/></svg></div>
          <div><span>大地图点位</span><p>在游戏大地图显示点位</p></div>
          ${toggle('map', '大地图点位')}
        </div>
        <div class="ae-quick-item">
          <div class="ae-quick-icon" aria-hidden="true"><svg viewBox="0 0 24 24"><path d="M20 6L9 17l-5-5"/></svg></div>
          <div><span>显示已完成点位</span><p>不影响已有完成记录</p></div>
          ${toggle('completed', '显示已完成点位')}
        </div>
      </div>
    `, 'QUICK OVERLAY // S01')}
    ${panel('筛选与记录本', `
      <div class="setting-row" style="padding-top:4px"><span>当前激活记录本</span><b>▧ 默认</b></div>
      <div class="setting-row"><span>已选点位类型</span><b>已选 ${state.selected.size} 类 / 共 ${pointData.length} 类</b></div>
      <div class="ae-selected-pills">
        ${selectedList.slice(0, 5).map(x => `<span class="ae-chip-tag"><i>✦</i>${esc(x)}</span>`).join('') || '<span class="tiny">暂未勾选点位类型</span>'}
        ${selectedList.length > 5 ? `<span class="ae-chip-tag">+${selectedList.length - 5}</span>` : ''}
      </div>
      <div class="actions" style="justify-content:space-between">
        <button data-action="profile">切换记录本</button>
        <a href="#filter" style="color:var(--accent);font-weight:700;font-size:12px">调整筛选 →</a>
      </div>
    `, 'ACTIVE PROFILE // F02')}
  </div>`;
}

// 06 路线页高定视图（左列航线卡 + 右列实时矢量蓝图检视舱）
function aeRoutes() {
  const activeRoute = routeData[state.route] || routeData[0];
  const pct = Math.round((activeRoute.done / activeRoute.count) * 100);
  const filteredRoutes = routeData.map((r, i) => ({ r, i })).filter(({ r }) => state.collection === '全部合集' || r.group === state.collection);
  return heading('路线', '整理已保存的路线与合集。选点和导航在游戏内进行。', 'ROUTES') + `
  <div class="toolbar">
    <label for="collection">合集</label>
    <select id="collection">${['全部合集', '日常探索', '收集物'].map(x => `<option${state.collection === x ? ' selected' : ''}>${x}</option>`).join('')}</select>
    ${button('新建合集', 'collection-new')}
    ${button('管理合集', 'collection-manage')}
    <span style="flex:1"></span>
    ${button('导入路线', 'route-import')}
    ${button('导出合集', 'route-export')}
    ${button(state.batch ? '退出批量' : '批量管理', 'batch')}
  </div>
  <div class="ae-routes-layout">
    ${panel('路线列表', `
      <div class="ae-route-cards">
        ${filteredRoutes.map(({ r, i }) => `
          <div class="ae-route-card-item ${state.route === i ? 'selected' : ''}" data-pick-route="${i}">
            <div>
              ${state.batch
                ? `<input type="checkbox" data-check="${i}" aria-label="选择${esc(r.name)}"${state.checked.has(i) ? ' checked' : ''}>`
                : `<span class="ae-route-serial">0${i + 1}</span>`}
            </div>
            <div>
              <div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">
                <b style="font-size:14px">${esc(r.name)}</b>
                <span class="badge">${r.kind}</span>
                ${i === 0 ? '<span class="badge" style="border-color:var(--accent-cyan);color:var(--accent-cyan)">当前路线</span>' : ''}
              </div>
              <p class="tiny" style="margin-top:5px">所属合集：${r.group} · 扇区坐标 ${r.coords}</p>
            </div>
            <div style="text-align:right">
              <b style="font:800 15px Consolas;color:var(--accent)">${r.done} <span class="tiny">/ ${r.count}</span></b>
              <small style="display:block;font-size:10px;color:var(--muted);margin-top:4px">目标 ${r.count} 个</small>
            </div>
          </div>
        `).join('')}
      </div>
    `, `${filteredRoutes.length} ROUTES`)}
    ${panel('航线矢量蓝图检视', `
      <div style="display:flex;justify-content:space-between;align-items:center">
        <div>
          <h3 style="font-size:17px">${esc(activeRoute.name)}</h3>
          <p class="tiny" style="margin-top:4px">${activeRoute.group} · ${activeRoute.kind} · 完成率 ${pct}%</p>
        </div>
        <span class="badge">${activeRoute.done} / ${activeRoute.count} 已完成</span>
      </div>
      <svg class="ae-route-blueprint-svg" viewBox="0 0 360 145" aria-label="当前选中路线平面轨迹预览">
        <path d="M0 36H360M0 72H360M0 108H360M72 0V145M144 0V145M216 0V145M288 0V145" stroke="rgba(229,193,123,.08)" stroke-width="1"/>
        <path d="${activeRoute.path}" fill="none" stroke="rgba(229,193,123,.22)" stroke-width="6" stroke-linecap="round"/>
        <path class="ae-route-vec" d="${activeRoute.path}" fill="none" stroke="#e5c17b" stroke-width="2.2"/>
        ${(activeRoute.nodes || []).map(([nx, ny], ni) => `<circle cx="${nx}" cy="${ny}" r="${ni === 0 ? 5.5 : 4.2}" fill="${ni === 0 ? '#4ce0d2' : '#09101a'}" stroke="${ni === 0 ? '#09101a' : '#e5c17b'}" stroke-width="2"/><text x="${nx - 6}" y="${ny - 9}" fill="#e5c17b" font-size="8.5" font-family="Consolas">0${ni + 1}</text>`).join('')}
        <text x="14" y="22" fill="#e5c17b" font-size="9.5" font-family="Consolas">TRAJECTORY PREVIEW // ${activeRoute.coords}</text>
      </svg>
      <p class="tiny" style="margin-bottom:14px">删除路线不会删除点位完成记录。</p>
      <div class="actions">
        ${state.batch
          ? button('全选', 'batch-all') + button('移动所选', 'route-move') + button('导出所选', 'route-export')
          : button('排序说明', 'route-sort')}
        ${button(state.batch ? '删除所选' : '删除路线', 'route-delete')}
      </div>
    `, 'BLUEPRINT // R02')}
  </div>
  <div class="notice">游戏内大地图 → 地图工具台 → 路径自动规划。预览只表示平面访问顺序，不代表实际可通行路径。</div>
  <details><summary>游戏内路线操作与手绘功能</summary><p>选点、框选、套索、指定起点、预览、开始指引、暂停/继续、跳过、重新规划、刷怪采集、手绘、自动轮换均保留在游戏内。</p><div class="actions" style="margin-top:14px">${button('查看完整操作', 'overlay-features')}</div></details>`;
}

// 06 点位筛选页高定视图
function aeFilter() {
  const cats = ['全部类型', '收集物', '宝箱', '采集物', '敌人', '角色养成', '武器养成'];
  return heading('点位筛选', '选择想在地图上显示的类型，完成记录不受影响。', 'POINT FILTER') + `
  <div class="toolbar">
    <input id="search" type="search" placeholder="搜索点位或材料（如：声匣、宝箱、灯笼果…）" aria-label="搜索点位或材料" value="${esc(state.query)}">
    <select id="category" aria-label="点位分类" style="display:none">${cats.map(x => `<option${state.category === x ? ' selected' : ''}>${x}</option>`).join('')}</select>
    <button class="chip" id="selected-only" aria-pressed="${state.selectedOnly}">✦ 仅看已选</button>
  </div>
  <div class="ae-cat-pills" style="margin-bottom:18px" role="group" aria-label="点位分类快捷筛选">
    ${cats.map(x => `<button class="ae-cat-btn" data-ae-cat="${x}" aria-pressed="${state.category === x}">${x}</button>`).join('')}
  </div>
  ${['角色养成', '武器养成'].includes(state.category) ? `<div class="notice" style="margin:0 0 18px">当前演示采用${state.category}材料组合示例，真实预设来自点位目录。</div>` : ''}
  <div class="filter-summary">
    <span id="selection-summary"></span>
    <div class="actions">
      ${button('全选当前结果', 'filter-all')}
      ${button('取消选择当前结果', 'filter-clear')}
    </div>
  </div>
  <div class="items" id="items"></div>
  <div class="notice">演示修改即时生效，仅保留在当前页面会话中。正式客户端会保存筛选；筛选不会改变点位完成状态。</div>
  <details><summary>筛选同步与提示</summary><p>正常时显示简短保存状态。目录异常、同步失败时显示原因与“重试同步”入口。</p>${button('查看异常状态示例', 'filter-error')}</details>`;
}

// 06 诊断页高定视图
function aeDiagnostics() {
  return heading('诊断', '查看运行状态，采集问题线索。', 'DIAGNOSTICS') + `
  <div class="small-grid">
    <div class="ae-metric-card">
      <div><span class="tiny">CORE LINK // 核心连接</span><b>未接入</b></div>
      <span class="badge">STANDBY</span>
    </div>
    <div class="ae-metric-card">
      <div><span class="tiny">OCR ENGINE // 坐标识别后端</span><b>待报告</b></div>
      <span class="badge">PENDING</span>
    </div>
    <div class="ae-metric-card">
      <div><span class="tiny">RUNTIME // 运行环境</span><b>HTML 演示</b></div>
      <span class="badge" style="border-color:var(--accent-cyan);color:var(--accent-cyan)">SIMULATED</span>
    </div>
  </div>
  <div class="settings-grid" style="margin-top:18px">
    ${panel('短时完整采集', `<p class="tiny">正式客户端最多记录 3 分钟，可提前停止。</p><div class="actions" style="margin-top:18px">${button(state.capture ? '■ 提前停止' : '● 开始 3 分钟采集', 'capture', true)}${button('打开采集文件夹', 'local-folder')}</div><div class="notice">${state.capture ? '采集界面演示中；没有录制画面或写入文件。' : '此处仅模拟采集状态，不记录游戏画面。'}</div>`, 'CAPTURE // D02')}
    ${panel('日志与报告', `<div class="log">[UI]      2026-11-03 16:20:00 · 寰宇星图高定草图已载入\n[CORE]    未连接真实核心（演示沙盒模式）\n[OCR]     无实际后端报告\n[STATUS]  演示状态就绪</div><div class="actions">${button('打开日志', 'local-folder')}${button('诊断摘要', 'diagnostic-summary')}${button('崩溃报告', 'crash')}</div>`, 'TELEMETRY LOG // D03')}
  </div>
  <details><summary>高级诊断：性能对照与隔离</summary><p>隐藏叠加窗口、暂停呈现、每帧工作隔离开关。仅在定位问题时使用。</p><div class="actions" style="margin-top:12px">${button('查看诊断工具', 'diagnostic-tools')}</div></details>`;
}

// 06 帮助页高定视图
function aeHelp() {
  return heading('帮助', '入门与操作说明，按需展开。', 'HELP') + `
  <div class="settings-grid">
    ${panel('开始使用 · 三步启航', `
      <div class="ae-step-grid">
        <div class="ae-step-box"><span class="ae-step-num">STEP 01</span><h3>连接游戏</h3><p class="tiny" style="margin-top:6px">首页选择游戏窗口，然后开始探索。</p></div>
        <div class="ae-step-box"><span class="ae-step-num">STEP 02</span><h3>筛选点位</h3><p class="tiny" style="margin-top:6px">按类型、角色或武器选择目标。</p></div>
        <div class="ae-step-box"><span class="ae-step-num">STEP 03</span><h3>游戏内规划路线</h3><p class="tiny" style="margin-top:6px">打开大地图工具台，选点、预览并开始指引。</p></div>
      </div>
      <div class="actions" style="margin-top:18px">${button('手柄速查', 'gamepad-help')}${button('攻略与点位操作', 'guide-help')}</div>
    `, 'QUICK START // G01')}
    ${panel('更多帮助', `<details open><summary>路线、手绘与取消操作</summary><p>暂停保留路线；退出导航结束本次导航。选点不是完成点位，平面访问顺序不是通行路径。</p>${button('查看完整操作', 'overlay-features')}</details><details><summary>旧数据恢复</summary><p>扫描旧记录、未登记记录本和删除保留的副本。</p>${button('查看恢复流程', 'legacy')}</details><details><summary>关于 IMAO</summary><p>鸣潮地图探索辅助工具。程序版本 2026.11.1.108。此处是设计探索草图。</p><a href="https://github.com/kahvia-d/WWMAP-TOOLS" target="_blank" rel="noreferrer">项目源码 ↗</a></details><details><summary>本次功能整理</summary><p>44 组功能，包含桌面、游戏内工具及攻略窗口。候选分组与文案不改变真实实现。</p>${button('打开功能清单', 'inventory')}</details>`, 'REFERENCE // G02')}
  </div>`;
}

// 06 设置页高定视图（带编号的精密调校选项卡）
function aeSettings() {
  const tabKeys = Object.keys(settingsContent);
  return heading('设置', '按用途分组，常用选项直接调整。', 'PREFERENCES') + `
  <div class="ae-settings-tabs" role="tablist" aria-label="设置分组">
    ${tabKeys.map((t, i) => `
      <button class="ae-tab-card" data-settings-tab="${t}" aria-pressed="${t === state.settingsTab}">
        <div><small>CFG // 0${i + 1}</small><b>${t}</b></div>
        <span style="font:700 12px Consolas;color:var(--accent)">›</span>
      </button>
    `).join('')}
  </div>
  <div class="settings-grid">${settingsContent[state.settingsTab]()}</div>`;
}

const aetheriaViews = {
  home: aeHome,
  routes: aeRoutes,
  filter: aeFilter,
  settings: aeSettings,
  diagnostics: aeDiagnostics,
  help: aeHelp
};

// 同步侧边栏与顶部状态栏结构（01-05 使用原版结构，06 使用寰宇星图高定结构）
function renderChrome() {
  const aside = $('#app-aside');
  const header = $('#app-header');
  if (state.theme === 'aetheria') {
    document.body.dataset.aetheriaMode = state.aetheriaLight ? 'light' : 'dark';
    aside.innerHTML = `
      <a class="ae-brand" href="#home">
        <div class="ae-crest" aria-hidden="true">
          <svg viewBox="0 0 36 36"><circle cx="18" cy="18" r="15" stroke-dasharray="3 2" stroke-opacity=".55"/><path d="M8 26V11l6 4h8l6-4v15z"/><circle cx="14.5" cy="18.5" r="1.3" fill="currentColor"/><circle cx="21.5" cy="18.5" r="1.3" fill="currentColor"/><path d="M16 22.5h4"/></svg>
        </div>
        <div class="ae-brand-text"><b>IMAO</b><small>鸣潮星图探索终端</small></div>
      </a>
      <div class="ae-nav-section-label"><span>TERMINAL WORKSPACE</span><span>01-06</span></div>
      <nav id="nav" aria-label="主导航">
        ${pages.map(([id, name, path, code], i) => `
          <a href="#${id}" class="ae-nav-link${state.page === id ? ' active' : ''}"${state.page === id ? ' aria-current="page"' : ''}>
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="${path}"/></svg>
            <span><span class="ae-nav-title">${name}</span><span class="ae-nav-sub">${code}</span></span>
            <span class="ae-nav-idx">0${i + 1}</span>
          </a>
        `).join('')}
      </nav>
      <div class="ae-companion-box">
        <div class="ae-companion-top">
          <b><span class="status-dot"></span>阿猫 · 声波示波器</b>
          <span>${state.running ? '432.8 Hz' : 'STANDBY'}</span>
        </div>
        <canvas id="ae-wave-canvas" width="200" height="38" aria-label="实时共鸣声波示波器"></canvas>
        <div class="ae-companion-foot">
          <span>演示环境</span>
          <span>2026.11.1.108</span>
        </div>
      </div>`;
    header.innerHTML = `
      <div class="ae-header-left">
        <span class="ae-telemetry-pill${state.running ? ' live' : ''}">
          <span class="ae-pulse-dot"></span>
          <b id="run-status">${state.running ? '正在探索（演示）' : '等待开始'}</b>
        </span>
        <span class="ae-freq-tag">${state.running ? 'RESONANCE // 432.8 Hz' : 'IDLE // 0.0 Hz'}</span>
        <button class="ae-source-chip" data-action="window" title="点击切换游戏窗口来源">⌖ <span id="game-source">${esc(state.source)}</span> <span>⌄</span></button>
      </div>
      <div class="ae-header-right">
        <button class="ae-mode-btn" id="ae-mode-toggle" aria-pressed="${state.aetheriaLight}">◐ ${state.aetheriaLight ? '凝脂白昼' : '曜金暗夜'}</button>
        <button class="profile" data-action="profile">▧ 默认记录本 <span>⌄</span></button>
      </div>`;
  } else {
    delete document.body.dataset.aetheriaMode;
    aside.innerHTML = `
      <a class="brand" href="#home"><svg viewBox="0 0 36 36" aria-hidden="true"><path d="M5 29V7l9 5h8l9-5v22zM11 20h4m7 0h4M16 26h5"/></svg><span>IMAO<small>鸣潮探索工具</small></span></a>
      <div class="nav-tag">工作区</div>
      <nav id="nav" aria-label="主导航">
        ${pages.map(([id, name, path]) => `<a href="#${id}"${state.page === id ? ' class="active" aria-current="page"' : ''}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="${path}"/></svg>${name}</a>`).join('')}
      </nav>
      <div class="aside-bottom"><div class="device-art" aria-hidden="true"><i></i><i></i><i></i><span>IMAO / LINK</span></div><span class="status-dot"></span>演示环境<small>2026.11.1.108 · UI CONCEPT</small></div>`;
    header.innerHTML = `
      <span><span class="status-dot"></span><b id="run-status">${state.running ? '正在探索（演示）' : '等待开始'}</b><span class="divider">/</span><span id="game-source">${esc(state.source)}</span></span>
      <button class="profile" data-action="profile">▧ 默认记录本 <span>⌄</span></button>`;
  }
}

// 切换风格并重绘当前视图（保留全部页面状态）
function applyTheme(t) {
  state.theme = t;
  document.body.dataset.theme = t;
  const x = themes.find(x => x[0] === t);
  $('#theme-name').textContent = x[1];
  $('#theme-note').textContent = x[3];
  $('#footer-theme').textContent = `0${themes.indexOf(x) + 1} / ${x[2]}`;
  document.querySelectorAll('[data-theme-select]').forEach(b => b.setAttribute('aria-pressed', b.dataset.themeSelect === t));
  render();
}

// 主渲染入口
function render() {
  state.page = pages.some(p => p[0] === location.hash.slice(1)) ? location.hash.slice(1) : 'home';
  renderChrome();
  const activeViews = state.theme === 'aetheria' ? aetheriaViews : views;
  $('#main').innerHTML = activeViews[state.page]();
  if (state.page === 'filter') paintItems();
  bind();
}

function visiblePoints() {
  return pointData.filter(p =>
    (state.category === '全部类型' || p[1] === state.category || (['角色养成', '武器养成'].includes(state.category) && p[1] === '采集物')) &&
    p[0].includes(state.query) &&
    (!state.selectedOnly || state.selected.has(p[0]))
  );
}

// 渲染点位筛选卡片（06 主题采用定制 SVG 纹章与高定收藏卡，01-05 保持原版）
function paintItems() {
  const rows = visiblePoints();
  $('#selection-summary').textContent = `已选 ${state.selected.size} 类 / 共 ${pointData.length} 类 · 当前 ${rows.length} 类`;
  if (state.theme === 'aetheria') {
    $('#items').innerHTML = rows.length
      ? rows.map(p => {
          const c = pointAccentColor(p[0], p[1]);
          const w = Math.min(100, Math.max(12, Math.round((p[3] / 128) * 100)));
          return `<button class="item ae-collector-card" style="--item-color:${c}" data-point="${p[0]}" aria-pressed="${state.selected.has(p[0])}">
            <span class="ae-emblem-box" aria-hidden="true">${pointEmblemSvg(p[0], p[1])}</span>
            <span style="min-width:0"><b style="font-size:13.5px;display:block">${p[0]}</b><small>${p[1]} · ${p[3]} 示例点</small><span class="ae-density-bar" aria-hidden="true"><i style="width:${w}%"></i></span></span>
            <span class="tick">${state.selected.has(p[0]) ? '✓' : '＋'}</span>
          </button>`;
        }).join('')
      : '<p>没有符合条件的点位，请调整搜索或分类。</p>';
  } else {
    $('#items').innerHTML = rows.length
      ? rows.map(p => `<button class="item" data-point="${p[0]}" aria-pressed="${state.selected.has(p[0])}"><span class="item-art" aria-hidden="true">${p[2]}</span><span>${p[0]}<small>${p[1]} · ${p[3]} 示例点</small></span><span class="tick">${state.selected.has(p[0]) ? '✓' : '＋'}</span></button>`).join('')
      : '<p>没有符合条件的点位，请调整搜索或分类。</p>';
  }
  document.querySelectorAll('[data-point]').forEach(b => b.onclick = () => {
    state.selected.has(b.dataset.point) ? state.selected.delete(b.dataset.point) : state.selected.add(b.dataset.point);
    paintItems();
  });
}

// 绑定页面交互事件
function bind() {
  document.querySelectorAll('.app [data-action]').forEach(b => b.onclick = () => action(b.dataset.action));
  document.querySelectorAll('[data-nav]').forEach(b => b.onclick = () => location.hash = b.dataset.nav);
  document.querySelectorAll('[data-toggle]').forEach(b => b.onclick = () => {
    const k = b.dataset.toggle;
    state.toggles[k] = !state.toggles[k];
    b.setAttribute('aria-checked', state.toggles[k]);
  });
  document.querySelectorAll('[data-value]').forEach(e => e.onchange = () => {
    if (e.type === 'number' && !e.checkValidity()) { e.reportValidity(); return; }
    state.values[e.dataset.value] = e.value;
  });
  document.querySelectorAll('[data-settings-tab]').forEach(b => b.onclick = () => {
    state.settingsTab = b.dataset.settingsTab;
    render();
  });
  document.querySelectorAll('[data-pick-route]').forEach(b => b.onclick = e => {
    if (e.target.matches('input[type="checkbox"]')) return;
    state.route = Number(b.dataset.pickRoute);
    render();
  });
  document.querySelectorAll('[data-check]').forEach(b => b.onchange = () => {
    const i = Number(b.dataset.check);
    b.checked ? state.checked.add(i) : state.checked.delete(i);
  });
  // 06 专属交互绑定：暗夜/白昼切换、星图图层开关、航点点击检视、分类胶囊
  if ($('#ae-mode-toggle')) {
    $('#ae-mode-toggle').onclick = () => {
      state.aetheriaLight = !state.aetheriaLight;
      render();
      toast(state.aetheriaLight ? '已切换至「凝脂白昼」高定色盘' : '已切换至「曜金暗夜」高定色盘');
    };
  }
  document.querySelectorAll('[data-map-layer]').forEach(b => b.onclick = () => {
    const k = b.dataset.mapLayer;
    state.mapLayers[k] = !state.mapLayers[k];
    render();
  });
  document.querySelectorAll('[data-wp]').forEach(g => {
    g.onclick = () => {
      state.activeWaypoint = Number(g.dataset.wp);
      render();
    };
    g.onkeydown = e => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        state.activeWaypoint = Number(g.dataset.wp);
        render();
      }
    };
  });
  document.querySelectorAll('[data-ae-cat]').forEach(b => b.onclick = () => {
    state.category = b.dataset.aeCat;
    render();
  });
  if ($('#collection')) $('#collection').onchange = e => { state.collection = e.target.value; render(); };
  if ($('#search')) {
    $('#search').oninput = e => { state.query = e.target.value; paintItems(); };
    $('#category').onchange = e => { state.category = e.target.value; render(); };
    $('#selected-only').onclick = () => { state.selectedOnly = !state.selectedOnly; render(); };
  }
}

let toastTimer;
function toast(s) {
  $('#toast').textContent = s;
  $('#toast').classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('#toast').classList.remove('show'), 3000);
}
function modal(title, body) {
  $('#dialog-content').innerHTML = `<div class="eyebrow">IMAO / INTERFACE PREVIEW</div><h2>${title}</h2>${body}`;
  if (!$('#dialog').open) $('#dialog').showModal();
}
function inventory(prefix = '') {
  const rows = window.FUNCTIONS.filter(r => !prefix || r.id.startsWith(prefix));
  modal(prefix === 'O' ? '游戏内功能 · 保留原有操作位置' : '现有功能清单 · 44 组', `<p>按任务合并重复入口。分组来自当前源码，未把草图等同于真实游戏功能验证。</p><input id="inventory-search" type="search" aria-label="搜索功能清单" placeholder="搜索功能或操作…"><div id="inventory-results" class="inventory-list"></div>`);
  function paint(q = '') {
    $('#inventory-results').innerHTML = rows.filter(r => (r.name + r.features + r.source).includes(q)).map(r => `<div class="inventory-row"><b><span class="id">${r.id}</span>${r.name}</b><p>${r.features}</p><small>当前入口：${r.source}</small></div>`).join('') || '<p>没有找到匹配的功能。</p>';
  }
  paint();
  $('#inventory-search').oninput = e => paint(e.target.value);
}

const info = {
  'reconnect': ['重新连接核心', '正式客户端会重新连接核心服务。当前草图没有核心连接，仅展示恢复操作入口。'],
  'profile': ['默认记录本', '记录本用于隔离点位完成记录与路线。切换、新建和修改均位于设置 → 记录与同步。'],
  'collection-manage': ['管理合集', '选择合集后可重命名或删除。当前是示例路线集；不修改真实路线文件。'],
  'collection-new': ['新建合集', '生产界面提供合集名称输入，之后保存的路线归入当前合集。草图展示操作位置，不新建磁盘文件。'],
  'route-import': ['导入路线包', '选择路线包后查看导入摘要；重名时可覆盖或新建。此草图不读取本地路线文件。'],
  'route-export': ['导出路线', '正式客户端支持导出当前合集或批量所选。此处展示导出前摘要，不生成真实路线包。'],
  'route-sort': ['调整路线顺序', '正式客户端支持拖动排序，Esc 或移出列表取消；自动轮换使用保存的顺序。此草图保留稳定顺序以便比较六种风格。'],
  'route-move': ['移动所选路线', '正式客户端可将所选路线移动到另一个合集。此处不修改真实合集归属。'],
  'route-delete': ['删除路线', '只删除路线文件，不影响点位完成记录。正在导航的路线被删除时会退出导航。本草图不执行删除。'],
  'shortcuts': ['全部快捷键', '开始/停止探索；完成点位；记录手绘线段端点；打开/关闭攻略；攻略内长按跳过目标；攻略上一张/下一张。保留保存、恢复默认与冲突提示。'],
  'gamepad-help': ['手柄速查', '大地图：LB/L1 打开工具台，RB/R1 打开点位助手；方向键/左摇杆选择，A/× 确认，B/○ 返回。普通探索：LB+B 完成附近点，LB+X 打开附近攻略。详情中长按 A 完成，长按 Y 跳过当前路线目标。'],
  'guide-help': ['攻略与点位操作', '标记点击查看攻略；重叠点先展开。支持图文、翻图、放大、刷新、来源链接、完成/撤销完成。选点模式下点击只增删路线目标，不改变完成记录。'],
  'profile-new': ['新建记录本', '填写名称，可选绑定库街区账号。一个库街区账号最多绑定一个记录本。此草图不采集账号信息。'],
  'profile-edit': ['修改记录本', '可修改名称与账号绑定。绑定不改变点位记录；本草图不读取或保存真实账号。'],
  'profile-delete': ['删除记录本', '正式客户端将记录本的点位与路线移到删除保留区，可恢复；不会一起删除账号凭据。本草图不执行删除。'],
  'profile-report': ['导出记录本检查报告', '查看记录本结构与异常线索，正式客户端可导出检查结果。本草图不访问真实数据。'],
  'legacy': ['旧数据恢复', '扫描旧单文件、未登记进度文档与已删除记录本，先看候选清单再恢复；原始文件保留，恢复结果不自动绑定库街区账号。'],
  'local-folder': ['打开本地目录', '正式客户端会打开对应的日志、采集、路线、同步或点位目录。HTML 草图不读取本机目录。'],
  'bridge': ['注册 / 修复浏览器桥接', '用于让已登录的库街区浏览器扩展连接桌面端。此处不注册程序或修改系统。'],
  'disconnect': ['断开库街区连接', '正式客户端会删除该账号本机保存的连接凭据；记录本与点位记录继续保留。此草图没有真实连接。'],
  'update-check': ['检查更新', '依次展示检查中、可更新、已是最新或失败。此草图未请求线上清单，不能确定是否存在新版。'],
  'offline': ['导入离线包', '生产界面选择并验证离线资源包后安装。此处不读取或安装文件。'],
  'program-download': ['下载程序', '展示来源、阶段、下载进度与取消操作。此草图不下载任何程序。'],
  'resources-download': ['下载地图资源', '资源安装后需完全退出并重启客户端启用。此草图不下载资源。'],
  'restart-update': ['退出并更新', '正式客户端退出后切换程序版本。草图不会退出或更新 IMAO。'],
  'cancel-download': ['取消下载', '正式客户端取消正在进行的下载。当前没有真实下载任务。'],
  'rollback-program': ['回退程序', '回退到可用的上一程序版本，需要明确确认。本草图不切换程序版本。'],
  'rollback-resource': ['回退地图资源', '回退上一资源版本，与程序回退分开。需要完全退出并重启。此处不修改资源。'],
  'repair-update': ['修复更新状态', '条件出现的恢复入口；镜像版本落后时还可切换 GitHub 并重新检查。本草图不执行修复。'],
  'connectivity': ['检测下载源', '正式客户端检测 GitHub 与 Mirror酱并显示连通性。草图不发起网络请求，状态保持未检测。'],
  'cdk': ['Mirror酱 CDK', '真实界面支持保存、清除 CDK 和打开服务站点。为避免误填，此草图不提供凭据输入框。'],
  'regions': ['管理区域资源', '按国家展开区域，显示本机副本大小与加载状态；启用/停用、下载/删除分别操作。停用保留文件，删除释放空间。这里不执行下载或删除。'],
  'compat': ['应用窗口兼容设置', '正式操作会影响全部 Direct3D 程序，需先明确影响范围。此草图不会修改 Windows。'],
  'compat-reset': ['恢复图形默认设置', '正式客户端恢复窗口兼容配置。本草图不修改系统设置。'],
  'diagnostic-summary': ['诊断摘要', '模拟环境：HTML 草图；核心未接入；OCR 后端未知；游戏窗口未读取。无法用此摘要判断真实客户端是否正常。'],
  'crash': ['崩溃报告', '正式客户端可打开崩溃报告目录。本草图没有采集崩溃信息。'],
  'diagnostic-tools': ['高级诊断工具', '隐藏叠加层窗口、暂停叠加层呈现；隔离值 0–1023 可分别控制采集、状态检测、定位、绘制、清屏、窗口几何、DComp、整帧回读、整屏覆盖与鼠标钩子。这些是对照诊断，不应放在玩家首页。'],
  'filter-error': ['筛选同步异常 · 示例', '示例状态：筛选尚未同步，已选类型保留。真实客户端会显示具体原因并提供重试同步。此处没有实际同步任务。']
};

function action(a) {
  if (a === 'inventory') return inventory();
  if (a === 'overlay-features') return inventory('O');
  if (a === 'start') {
    state.running = !state.running;
    render();
    return toast(state.running ? '已开始演示探索' : '已停止演示探索');
  }
  if (a === 'window') {
    modal('选择游戏窗口', `<p>支持本地客户端或云鸣潮浏览器窗口。以下均为模拟选项。</p><div class="actions"><button id="local-choice">本地客户端</button><button id="cloud-choice">云鸣潮浏览器窗口</button></div><p>正式客户端可刷新窗口列表，并预览自动识别的游戏区域。</p>`);
    for (const [id, value] of [['local-choice', '本地客户端'], ['cloud-choice', '云鸣潮浏览器窗口']]) {
      $('#' + id).onclick = () => {
        state.source = value;
        $('#dialog').close();
        render();
        toast('已切换模拟游戏来源');
      };
    }
    return;
  }
  if (a === 'batch') { state.batch = !state.batch; return render(); }
  if (a === 'batch-all') {
    routeData.forEach((r, i) => { if (state.collection === '全部合集' || state.collection === r.group) state.checked.add(i); });
    return render();
  }
  if (a === 'filter-all' || a === 'filter-clear') {
    visiblePoints().forEach(p => a === 'filter-all' ? state.selected.add(p[0]) : state.selected.delete(p[0]));
    return paintItems();
  }
  if (a === 'capture') { state.capture = !state.capture; return render(); }
  if (a === 'save-shortcuts') return toast('已保留演示按键选择；未写入实际配置');
  if (a === 'refresh-pad') return toast('未读取真实设备；展示模拟手柄列表');
  if (a === 'sync-preview') {
    state.syncPreview = true;
    render();
    modal('同步预览 · 模拟数据', '<p>默认记录本 · 全部区域。以下仅用于展示差异表。</p><div class="log">本地独有完成：12\n库街区独有完成：8\n已同步：320\n将被取消：0</div><p>真实同步基于账号绑定与差异确认。此处没有真实账号或同步计划。</p>');
    return;
  }
  if (a === 'sync-apply') return toast('模拟同步已完成，未向任何服务发送数据');
  const x = info[a];
  if (x) modal(x[0], `<p>${x[1]}</p>${a === 'route-export' ? `<div class="log">${state.batch ? `所选 ${state.checked.size} 条示例路线` : `合集：${esc(state.collection)}`}\n不生成真实文件</div>` : ''}`);
}

// 06 侧边栏阿猫实时声波示波器绘制循环
let wavePhase = 0;
function drawAetheriaWave() {
  if (state.theme === 'aetheria') {
    const cvs = $('#ae-wave-canvas');
    if (cvs) {
      const ctx = cvs.getContext('2d');
      const w = cvs.width;
      const h = cvs.height;
      ctx.clearRect(0, 0, w, h);
      const speed = state.running ? 0.14 : 0.04;
      const amp = state.running ? 11 : 4.5;
      wavePhase += speed;
      // 主谐波（曜金）
      ctx.beginPath();
      for (let x = 0; x <= w; x += 2) {
        const env = Math.sin((x / w) * Math.PI);
        const y = h / 2 + Math.sin(x * 0.06 + wavePhase) * amp * env;
        x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      }
      ctx.strokeStyle = '#e5c17b';
      ctx.lineWidth = 1.6;
      ctx.stroke();
      // 次谐波（泰缇斯青）
      ctx.beginPath();
      for (let x = 0; x <= w; x += 2) {
        const env = Math.sin((x / w) * Math.PI);
        const y = h / 2 + Math.cos(x * 0.09 - wavePhase * 1.3) * (amp * 0.65) * env;
        x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      }
      ctx.strokeStyle = '#4ce0d2';
      ctx.lineWidth = 1.1;
      ctx.stroke();
    }
  }
  if (!window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    requestAnimationFrame(drawAetheriaWave);
  }
}

// 初始化顶部 6 个风格选项按钮（第 6 项附带高定标识）
$('#themes').innerHTML = themes.map((t, i) => `
  <button data-theme-select="${t[0]}" aria-pressed="${t[0] === state.theme}">
    <span class="swatch ${t[0]}"></span>
    <span>
      <b>${t[1]}${t[0] === 'aetheria' ? '<span class="ae-pill-tag">✦ NEW</span>' : ''}</b>
      <small>0${i + 1} / ${t[2]}</small>
    </span>
  </button>
`).join('');

document.querySelectorAll('[data-theme-select]').forEach(b => b.onclick = () => applyTheme(b.dataset.themeSelect));
$('#inventory-open').onclick = () => inventory();
$('#close').onclick = () => $('#dialog').close();
window.addEventListener('hashchange', render);
window.addEventListener('keydown', e => {
  if (e.key === 'F9' && !$('#dialog').open) {
    e.preventDefault();
    action('start');
  }
});

// 默认激活 06 寰宇星图高定版，方便打开即见并可随时点击 01-05 切换对比
applyTheme('aetheria');
drawAetheriaWave();
