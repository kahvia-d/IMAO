import { originPatternForBaseUrl, validateBaseUrl } from "./webdav-crypto.js";

const enabledEl = document.querySelector("#enabled");
const configAreaEl = document.querySelector("#configArea");
const baseUrlEl = document.querySelector("#baseUrl");
const usernameEl = document.querySelector("#username");
const passwordEl = document.querySelector("#password");
const passphraseEl = document.querySelector("#passphrase");
const autoUploadEl = document.querySelector("#autoUploadCredentials");
const autoPromptEl = document.querySelector("#autoPromptRecovery");
const saveBtn = document.querySelector("#saveBtn");
const testBtn = document.querySelector("#testBtn");
const copyBundleBtn = document.querySelector("#copyBundleBtn");
const pasteBundleBtn = document.querySelector("#pasteBundleBtn");
const noticeEl = document.querySelector("#notice");
const accountsCardEl = document.querySelector("#accountsCard");
const accountsListEl = document.querySelector("#accountsList");
const refreshAccountsBtn = document.querySelector("#refreshAccountsBtn");

function showNotice(kind, text) {
  noticeEl.className = `notice ${kind}`;
  noticeEl.textContent = text;
}

function clearNotice() {
  noticeEl.className = "notice";
  noticeEl.textContent = "";
}

function readDraft() {
  return {
    enabled: enabledEl.checked,
    baseUrl: baseUrlEl.value.trim(),
    username: usernameEl.value.trim(),
    password: passwordEl.value,
    passphrase: passphraseEl.value,
    autoUploadCredentials: autoUploadEl.checked,
    autoPromptRecovery: autoPromptEl.checked
  };
}

function formatDevice(source) {
  if (source === "pc") return "PC 端";
  if (source === "browser") return "浏览器扩展";
  if (source === "pocket") return "移动端";
  return source || "其它设备";
}

function formatTime(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString("zh-CN");
}

function renderAccounts(accounts) {
  accountsListEl.innerHTML = "";
  if (!accounts || accounts.length === 0) {
    accountsListEl.innerHTML = '<p class="sub">云端目前没有已保存的库街区账号凭证。</p>';
    return;
  }
  for (const acc of accounts) {
    const row = document.createElement("div");
    row.className = "account-row";

    const info = document.createElement("div");
    info.className = "account-info";
    const title = document.createElement("strong");
    title.textContent = acc.displayName ? `${acc.displayName}（${acc.accountId}）` : `账号 ${acc.accountId}`;
    const sub = document.createElement("span");
    sub.textContent = `版本 v${acc.version} · 来源：${formatDevice(acc.sourceDevice)} · 更新于 ${formatTime(acc.updatedAtUtc)}`;
    info.appendChild(title);
    info.appendChild(sub);

    const delBtn = document.createElement("button");
    delBtn.className = "danger";
    delBtn.textContent = "清除云端";
    delBtn.addEventListener("click", async () => {
      if (!confirm(`确定要从 WebDAV 删除账号 ${acc.accountId} 的云端凭证吗？不会影响本机或大地图现有进度。`)) return;
      delBtn.disabled = true;
      const resp = await chrome.runtime.sendMessage({ type: "webdavDeleteAccount", accountId: acc.accountId });
      if (resp?.error) {
        showNotice("err", `清除失败：${resp.error}`);
        delBtn.disabled = false;
        return;
      }
      showNotice("ok", `已从 WebDAV 清除账号 ${acc.accountId} 的云端凭证。`);
      await loadAccounts();
    });

    row.appendChild(info);
    row.appendChild(delBtn);
    accountsListEl.appendChild(row);
  }
}

async function loadAccounts() {
  refreshAccountsBtn.disabled = true;
  const resp = await chrome.runtime.sendMessage({ type: "webdavListAccounts" });
  refreshAccountsBtn.disabled = false;
  if (resp?.error) {
    showNotice("err", `读取云端账号列表失败：${resp.error}`);
    return;
  }
  renderAccounts(resp?.accounts || []);
}

async function requestHostPermission(baseUrl) {
  const originPattern = originPatternForBaseUrl(baseUrl);
  const already = await chrome.permissions.contains({ origins: [originPattern] });
  if (already) return true;
  return await chrome.permissions.request({ origins: [originPattern] });
}

enabledEl.addEventListener("change", async () => {
  const enabled = enabledEl.checked;
  configAreaEl.classList.toggle("hidden", !enabled);
  if (!enabled) {
    accountsCardEl.classList.add("hidden");
    const resp = await chrome.runtime.sendMessage({
      type: "webdavSaveSettings",
      settings: readDraft()
    });
    if (resp?.error) showNotice("err", `保存停用状态失败：${resp.error}`);
    else showNotice("info", "已关闭 WebDAV 凭证同步。扩展不会再访问任何 WebDAV 服务。");
  } else {
    clearNotice();
  }
});

saveBtn.addEventListener("click", async () => {
  const draft = readDraft();
  if (!draft.baseUrl || !draft.username || !draft.password || !draft.passphrase) {
    showNotice("err", "请完整填写 WebDAV 地址、用户名、应用密码和同步加密口令。");
    return;
  }
  try {
    draft.baseUrl = validateBaseUrl(draft.baseUrl);
    baseUrlEl.value = draft.baseUrl;
  } catch (error) {
    showNotice("err", error.message);
    return;
  }

  saveBtn.disabled = true;
  try {
    // 仅动态申请玩家自己填写的那一个 WebDAV 域名权限
    const granted = await requestHostPermission(draft.baseUrl);
    if (!granted) {
      showNotice("err", "浏览器拒绝了对该 WebDAV 域名的访问授权，无法同步。");
      return;
    }
    const resp = await chrome.runtime.sendMessage({ type: "webdavSaveSettings", settings: draft });
    if (resp?.error) {
      showNotice("err", `保存失败：${resp.error}`);
      return;
    }
    showNotice("ok", "已保存 WebDAV 配置并完成域名授权。可以点击「测试连接」验证连通性。");
    accountsCardEl.classList.remove("hidden");
  } catch (error) {
    showNotice("err", `保存时发生异常：${error?.message || error}`);
  } finally {
    saveBtn.disabled = false;
  }
});

testBtn.addEventListener("click", async () => {
  const draft = readDraft();
  if (!draft.baseUrl || !draft.username || !draft.password || !draft.passphrase) {
    showNotice("err", "请先完整填写四项配置。");
    return;
  }
  try {
    draft.baseUrl = validateBaseUrl(draft.baseUrl);
    baseUrlEl.value = draft.baseUrl;
  } catch (error) {
    showNotice("err", error.message);
    return;
  }

  testBtn.disabled = true;
  try {
    const granted = await requestHostPermission(draft.baseUrl);
    if (!granted) {
      showNotice("err", "未授予该 WebDAV 域名的访问权限，无法测试连接。");
      return;
    }
    showNotice("info", "正在测试 WebDAV 连接并检查同步目录…");
    const resp = await chrome.runtime.sendMessage({ type: "webdavTestConnection", settings: draft });
    if (resp?.error) {
      showNotice("err", `连接测试失败：${resp.error}`);
      return;
    }
    const accounts = resp.accounts || [];
    accountsCardEl.classList.remove("hidden");
    renderAccounts(accounts);
    showNotice("ok", accounts.length === 0
      ? "WebDAV 连接成功，同步目录已就绪（云端暂无账号凭证）。"
      : `WebDAV 连接成功，云端已有 ${accounts.length} 个库街区账号的加密凭证。`);
  } catch (error) {
    showNotice("err", `连接测试异常：${error?.message || error}`);
  } finally {
    testBtn.disabled = false;
  }
});

copyBundleBtn.addEventListener("click", async () => {
  const draft = readDraft();
  if (!draft.baseUrl || !draft.username || !draft.password || !draft.passphrase) {
    showNotice("err", "请先填完四项配置，再复制给 PC 端 IMao 使用。");
    return;
  }
  const resp = await chrome.runtime.sendMessage({ type: "webdavEncodeBundle", bundle: draft });
  if (resp?.error) {
    showNotice("err", `生成配置文本失败：${resp.error}`);
    return;
  }
  await navigator.clipboard.writeText(resp.text);
  showNotice("info", "已复制便携配置文本到剪贴板（含密码与口令，请仅粘贴给自己的 IMao 客户端，切勿外传）。");
});

pasteBundleBtn.addEventListener("click", async () => {
  try {
    const text = await navigator.clipboard.readText();
    const resp = await chrome.runtime.sendMessage({ type: "webdavDecodeBundle", text });
    if (resp?.error) {
      showNotice("err", resp.error);
      return;
    }
    baseUrlEl.value = resp.bundle.baseUrl;
    usernameEl.value = resp.bundle.username;
    passwordEl.value = resp.bundle.password;
    passphraseEl.value = resp.bundle.passphrase;
    showNotice("ok", "已填入便携配置文本，请点击「保存并授权域名」。");
  } catch (error) {
    showNotice("err", `读取剪贴板失败：${error?.message || error}`);
  }
});

refreshAccountsBtn.addEventListener("click", loadAccounts);

// 初始化加载已保存配置
(async () => {
  const resp = await chrome.runtime.sendMessage({ type: "webdavGetSettings" });
  const s = resp?.settings;
  if (!s) return;
  enabledEl.checked = Boolean(s.enabled);
  baseUrlEl.value = s.baseUrl || "";
  usernameEl.value = s.username || "";
  passwordEl.value = s.password || "";
  passphraseEl.value = s.passphrase || "";
  autoUploadEl.checked = s.autoUploadCredentials !== false;
  autoPromptEl.checked = s.autoPromptRecovery !== false;
  configAreaEl.classList.toggle("hidden", !s.enabled);
  if (resp.ready && resp.permissionGranted) {
    accountsCardEl.classList.remove("hidden");
    await loadAccounts();
  }
})();
