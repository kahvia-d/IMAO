const statusEl = document.querySelector("#status");
const connectBtn = document.querySelector("#connect");
const webdavSection = document.querySelector("#webdavSection");
const uploadWebDavBtn = document.querySelector("#uploadWebDav");
const refreshWebDavBtn = document.querySelector("#refreshWebDav");
const accountListEl = document.querySelector("#accountList");

document.querySelector("#openOptions").addEventListener("click", () => {
  chrome.runtime.openOptionsPage();
});

connectBtn.addEventListener("click", async () => {
  connectBtn.disabled = true;
  statusEl.textContent = "正在连接桌面端…";
  try {
    const response = await chrome.runtime.sendMessage({ type: "connect" });
    if (response?.error) {
      statusEl.textContent = `连接失败：${response.error}`;
      return;
    }
    let extra = "";
    if (response.webdavUploaded) {
      extra = `\n并已同步加密上传到 WebDAV（v${response.webdavVersion}）。`;
    } else if (response.webdavError) {
      extra = `\n（顺带上传 WebDAV 失败：${response.webdavError}，可稍后点击下方手动上传）`;
    }
    statusEl.textContent =
      `已连接库街区账号 ${response.accountId}。${extra}\n\n回到 IMao → 设置 → 本地点位记录本 → 点「修改」，把 ${response.accountId} 填进「绑定库街区账号」。`;
    if (response.webdavUploaded) await refreshAccounts();
  } finally {
    connectBtn.disabled = false;
  }
});

function formatDevice(source) {
  if (source === "pc") return "PC 端";
  if (source === "browser") return "浏览器";
  if (source === "pocket") return "移动端";
  return source || "其它设备";
}

function formatTime(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "";
  const mm = String(d.getMonth() + 1).padStart(2, "0");
  const dd = String(d.getDate()).padStart(2, "0");
  const hh = String(d.getHours()).padStart(2, "0");
  const mi = String(d.getMinutes()).padStart(2, "0");
  return `${mm}-${dd} ${hh}:${mi}`;
}

function renderAccounts(accounts) {
  accountListEl.innerHTML = "";
  if (!accounts || accounts.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-hint";
    empty.textContent = "WebDAV 中还没有已上传的账号凭证。";
    accountListEl.appendChild(empty);
    return;
  }

  for (const acc of accounts) {
    const row = document.createElement("div");
    row.className = "account-item";

    const meta = document.createElement("div");
    meta.className = "account-meta";
    const name = document.createElement("div");
    name.className = "account-name";
    name.textContent = acc.displayName ? `${acc.displayName}（${acc.accountId}）` : `账号 ${acc.accountId}`;
    const sub = document.createElement("div");
    sub.className = "account-sub";
    sub.textContent = `v${acc.version} · ${formatDevice(acc.sourceDevice)} · ${formatTime(acc.updatedAtUtc)}`;
    meta.appendChild(name);
    meta.appendChild(sub);

    const btn = document.createElement("button");
    btn.className = "account-btn";
    btn.textContent = "切换 / 恢复";
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      statusEl.textContent = `正在解密并恢复账号 ${acc.accountId}…`;
      const resp = await chrome.runtime.sendMessage({
        type: "webdavApplyToCurrentTab",
        accountId: acc.accountId
      });
      if (resp?.error) {
        statusEl.textContent = `恢复失败：${resp.error}`;
        btn.disabled = false;
      } else {
        statusEl.textContent = `已写入账号 ${acc.accountId}（v${resp.version}），正在刷新大地图…`;
        setTimeout(() => window.close(), 700);
      }
    });

    row.appendChild(meta);
    row.appendChild(btn);
    accountListEl.appendChild(row);
  }
}

async function refreshAccounts() {
  refreshWebDavBtn.disabled = true;
  accountListEl.innerHTML = '<div class="empty-hint">正在读取 WebDAV 账号列表…</div>';
  const resp = await chrome.runtime.sendMessage({ type: "webdavListAccounts" });
  refreshWebDavBtn.disabled = false;
  if (resp?.error) {
    accountListEl.innerHTML = "";
    statusEl.textContent = `读取 WebDAV 失败：${resp.error}`;
    return;
  }
  renderAccounts(resp?.accounts || []);
}

uploadWebDavBtn.addEventListener("click", async () => {
  uploadWebDavBtn.disabled = true;
  statusEl.textContent = "正在加密上传当前网页凭证到 WebDAV…";
  const resp = await chrome.runtime.sendMessage({ type: "webdavUploadCurrentTab" });
  uploadWebDavBtn.disabled = false;
  if (resp?.error) {
    statusEl.textContent = `上传失败：${resp.error}`;
    return;
  }
  statusEl.textContent = `已加密上传账号 ${resp.accountId} 的凭证到 WebDAV（v${resp.version}）。`;
  await refreshAccounts();
});

refreshWebDavBtn.addEventListener("click", refreshAccounts);

// 初始化：仅在用户开启了 WebDAV 时才展示 WebDAV 区域，未开启时对老用户完全零干扰
(async () => {
  const resp = await chrome.runtime.sendMessage({ type: "webdavGetSettings" });
  if (resp?.ready) {
    webdavSection.classList.remove("hidden");
    await refreshAccounts();
  }
})();
