// 隔离世界中继脚本：负责在 extract-main.js 与 service-worker.js 之间传递会话，
// 并在检测到掉线或 WebDAV 有更新凭证时，在页面右上角渲染隔离样式的恢复提示卡片。

const BANNER_HOST_ID = "imao-webdav-recovery-banner-host";
let bannerDismissed = false;

const forward = detail => {
  if (!detail || typeof detail.token !== "string") return;
  chrome.runtime.sendMessage({
    type: "sessionCandidate",
    accountId: String(detail.accountId || ""),
    displayName: String(detail.displayName || ""),
    token: detail.token,
    userInfoJson: typeof detail.userInfoJson === "string" ? detail.userInfoJson : "",
    diagnostic: String(detail.diagnostic || "")
  }).catch(() => { });
};

window.addEventListener("imao-kuro-session", event => forward(event.detail));

function formatDevice(source) {
  if (source === "pc") return "PC 端";
  if (source === "browser") return "浏览器扩展";
  if (source === "pocket") return "移动端";
  return source || "其它设备";
}

function removeBanner() {
  const existing = document.getElementById(BANNER_HOST_ID);
  if (existing) existing.remove();
}

function showRecoveryBanner({ reason, suggestedAccountId, accounts }) {
  if (bannerDismissed || !Array.isArray(accounts) || accounts.length === 0) return;
  removeBanner();

  const host = document.createElement("div");
  host.id = BANNER_HOST_ID;
  host.style.cssText = "position:fixed;top:16px;right:16px;z-index:2147483647;font-family:system-ui,-apple-system,sans-serif;";
  const shadow = host.attachShadow({ mode: "closed" });

  const selected = accounts.find(a => a.accountId === suggestedAccountId) || accounts[0];
  const titleText = reason === "updated"
    ? `检测到账号 ${selected.accountId} 有更新的登录凭证`
    : "检测到当前大地图未登录或已掉线";
  const descText = reason === "updated"
    ? `WebDAV 上存在来自「${formatDevice(selected.sourceDevice)}」的新凭证（v${selected.version}），当前浏览器会话可能已被顶下线。是否立即恢复登录？`
    : "WebDAV 中已有可用的库街区加密凭证，可一键恢复登录并刷新地图。";

  shadow.innerHTML = `
    <style>
      .card {
        width: 320px;
        background: #1f2329;
        color: #f2f4f8;
        border: 1px solid rgba(255,255,255,0.14);
        border-radius: 12px;
        box-shadow: 0 12px 32px rgba(0,0,0,0.45);
        padding: 14px 16px;
        font-size: 13px;
        line-height: 1.5;
      }
      .header { display: flex; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 6px; }
      .title { font-weight: 600; font-size: 13.5px; color: #ffffff; }
      .close { background: transparent; border: none; color: #9aa4b2; cursor: pointer; font-size: 16px; padding: 0 4px; }
      .close:hover { color: #ffffff; }
      .desc { color: #c7d0dc; margin: 0 0 10px 0; }
      select {
        width: 100%;
        box-sizing: border-box;
        margin-bottom: 10px;
        padding: 6px 8px;
        border-radius: 6px;
        border: 1px solid rgba(255,255,255,0.2);
        background: #2b313a;
        color: #f2f4f8;
        font-size: 12.5px;
      }
      .actions { display: flex; gap: 8px; }
      button.primary {
        flex: 1;
        background: #3b82f6;
        color: #fff;
        border: none;
        border-radius: 6px;
        padding: 7px 10px;
        font-size: 12.5px;
        font-weight: 600;
        cursor: pointer;
      }
      button.primary:hover { background: #2563eb; }
      button.primary:disabled { opacity: 0.6; cursor: default; }
      button.secondary {
        background: #313844;
        color: #d7dee8;
        border: none;
        border-radius: 6px;
        padding: 7px 12px;
        font-size: 12.5px;
        cursor: pointer;
      }
      button.secondary:hover { background: #3d4654; }
      .status { margin-top: 8px; font-size: 12px; color: #fca5a5; display: none; }
    </style>
    <div class="card">
      <div class="header">
        <span class="title">IMao 凭证同步提醒</span>
        <button class="close" id="btnClose" title="本次忽略">×</button>
      </div>
      <p class="desc" id="desc"></p>
      <select id="accountSelect" style="display:none"></select>
      <div class="actions">
        <button class="primary" id="btnApply">立即恢复登录</button>
        <button class="secondary" id="btnIgnore">暂不处理</button>
      </div>
      <div class="status" id="status"></div>
    </div>
  `;

  shadow.querySelector(".title").textContent = titleText;
  shadow.querySelector("#desc").textContent = descText;

  const select = shadow.querySelector("#accountSelect");
  if (accounts.length > 1) {
    select.style.display = "block";
    for (const acc of accounts) {
      const opt = document.createElement("option");
      opt.value = acc.accountId;
      const label = acc.displayName ? `${acc.displayName}（${acc.accountId}）` : `账号 ${acc.accountId}`;
      opt.textContent = `${label} · v${acc.version} · ${formatDevice(acc.sourceDevice)}`;
      if (acc.accountId === selected.accountId) opt.selected = true;
      select.appendChild(opt);
    }
  }

  const dismiss = () => {
    bannerDismissed = true;
    removeBanner();
  };
  shadow.querySelector("#btnClose").addEventListener("click", dismiss);
  shadow.querySelector("#btnIgnore").addEventListener("click", dismiss);

  const btnApply = shadow.querySelector("#btnApply");
  const statusEl = shadow.querySelector("#status");
  btnApply.addEventListener("click", async () => {
    const targetAccountId = accounts.length > 1 ? select.value : selected.accountId;
    btnApply.disabled = true;
    btnApply.textContent = "正在解密并恢复…";
    statusEl.style.display = "none";
    try {
      const resp = await chrome.runtime.sendMessage({
        type: "webdavApplyToCurrentTab",
        accountId: targetAccountId
      });
      if (resp?.error) {
        statusEl.textContent = `恢复失败：${resp.error}`;
        statusEl.style.display = "block";
        btnApply.disabled = false;
        btnApply.textContent = "立即恢复登录";
      }
    } catch (error) {
      statusEl.textContent = `恢复失败：${error?.message || error}`;
      statusEl.style.display = "block";
      btnApply.disabled = false;
      btnApply.textContent = "立即恢复登录";
    }
  });

  (document.body || document.documentElement).appendChild(host);
}

// 接收来自 service-worker 的写回指令与浮层提醒指令
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type === "applySessionToPage") {
    const onResult = event => {
      window.removeEventListener("imao-kuro-apply-result", onResult);
      sendResponse(event.detail || { ok: true });
    };
    window.addEventListener("imao-kuro-apply-result", onResult);
    window.dispatchEvent(new CustomEvent("imao-kuro-apply-session", {
      detail: {
        accountId: String(message.accountId || ""),
        token: String(message.token || ""),
        userInfoJson: typeof message.userInfoJson === "string" ? message.userInfoJson : ""
      }
    }));
    // 页面会立刻 reload，兜底返回避免消息通道悬挂
    setTimeout(() => {
      window.removeEventListener("imao-kuro-apply-result", onResult);
      sendResponse({ ok: true });
    }, 300);
    return true;
  }
  if (message?.type === "showRecoveryBanner") {
    showRecoveryBanner(message);
    sendResponse({ ok: true });
    return false;
  }
  return false;
});

// 立即并间隔重试请求页面上报会话（兼容单页应用延迟写入 localStorage）
const request = () => window.dispatchEvent(new CustomEvent("imao-kuro-session-request"));
request();
let attempts = 0;
const timer = setInterval(() => {
  request();
  if (++attempts >= 6) clearInterval(timer);
}, 1000);
