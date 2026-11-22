import { WebDavClient } from "./webdav-client.js";
import {
  decodeConfigBundle,
  encodeConfigBundle,
  isAccountId,
  originPatternForBaseUrl,
  validateBaseUrl
} from "./webdav-crypto.js";

const HOST_NAME = "com.imao.kuro_sync";
const WEBDAV_SETTINGS_KEY = "imaoWebDavSettings";
const MAP_URL_REGEX = /^https:\/\/(www\.)?kurobbs\.com\/mc\/map\//;

const sessionStorageKey = tabId => `candidate:${tabId}`;
const checkedTabKey = tabId => `webdavChecked:${tabId}`;

const DEFAULT_WEBDAV_SETTINGS = {
  enabled: false,
  baseUrl: "",
  username: "",
  password: "",
  passphrase: "",
  autoPromptRecovery: true
};

async function loadWebDavSettings() {
  const stored = (await chrome.storage.local.get(WEBDAV_SETTINGS_KEY))[WEBDAV_SETTINGS_KEY];
  return { ...DEFAULT_WEBDAV_SETTINGS, ...(stored || {}) };
}

function isWebDavReady(settings) {
  return Boolean(
    settings?.enabled &&
    settings.baseUrl?.trim() &&
    settings.username?.trim() &&
    settings.password &&
    settings.passphrase
  );
}

async function hasWebDavOriginPermission(baseUrl) {
  try {
    const originPattern = originPatternForBaseUrl(baseUrl);
    return await chrome.permissions.contains({ origins: [originPattern] });
  } catch {
    return false;
  }
}

/** 直接在目标大地图标签页的主世界现读当前会话（同时兼容 AKI_MAP_USER_PROFILE 与 AKI_MAP_USER_TOKEN） */
async function readLiveSessionFromTab(tabId) {
  try {
    const results = await chrome.scripting.executeScript({
      target: { tabId },
      world: "MAIN",
      func: () => {
        let token = localStorage.getItem("AKI_MAP_USER_TOKEN") || "";
        let userInfoJson = localStorage.getItem("AKI_MAP_USER_INFO") || "";
        const profileRaw = localStorage.getItem("AKI_MAP_USER_PROFILE") || "";
        let profileObj = null;
        try {
          const persisted = JSON.parse(profileRaw || "{}");
          if (persisted?.profile && typeof persisted.profile === "object") profileObj = persisted.profile;
          if (!token && typeof persisted?.token === "string") token = persisted.token;
          if (!token && typeof profileObj?.token === "string") token = profileObj.token;
        } catch { }
        if (!profileObj) {
          try {
            const parsed = JSON.parse(userInfoJson || "{}");
            if (parsed && typeof parsed === "object" && Object.keys(parsed).length > 0) profileObj = parsed;
          } catch { }
        }
        if (!userInfoJson && profileObj) {
          try { userInfoJson = JSON.stringify(profileObj); } catch { }
        }
        const accountId = String(profileObj?.userId ?? "");
        const displayName = String(profileObj?.userName ?? profileObj?.nickName ?? profileObj?.name ?? "");
        const keys = profileObj ? Object.keys(profileObj).slice(0, 12).join(",") : "";
        return {
          accountId,
          displayName,
          token,
          userInfoJson,
          diagnostic: `tokenLength=${token.length} userIdLength=${accountId.length} userInfoKeys=${keys}`
        };
      }
    });
    const session = results?.[0]?.result;
    if (session?.token?.length > 8) return session;
  } catch { }
  const cached = (await chrome.storage.session.get(sessionStorageKey(tabId)))[sessionStorageKey(tabId)];
  return cached?.token?.length > 8 ? cached : null;
}

// 与桌面端 KuroTokenVault.AccountCredentialId 保持逐字符一致的命名规则
function profileId(value) {
  const normalized = String(value || "").replace(/[^a-zA-Z0-9_-]/g, "").replace(/^kuro_/, "").slice(0, 80);
  return normalized ? `kuro_${normalized}` : "";
}

async function setBadge(tabId, text) {
  try {
    await chrome.action.setBadgeText({ tabId, text });
    if (text) await chrome.action.setBadgeBackgroundColor({ tabId, color: "#ef4444" });
  } catch { }
}

// 收到大地图页面上报的会话候选：先缓存，若启用了 WebDAV 自动提醒则每个标签页低频检查一次
chrome.runtime.onMessage.addListener((message, sender) => {
  if (message?.type !== "sessionCandidate" || sender.tab?.id == null || !MAP_URL_REGEX.test(sender.tab.url || "")) return;
  if (typeof message.token !== "string" || message.token.length > 16384) return;
  const tabId = sender.tab.id;
  const candidate = {
    accountId: String(message.accountId || ""),
    displayName: String(message.displayName || ""),
    token: message.token,
    userInfoJson: typeof message.userInfoJson === "string" ? message.userInfoJson : "",
    diagnostic: String(message.diagnostic || "")
  };
  chrome.storage.session.set({ [sessionStorageKey(tabId)]: candidate });
  void maybePromptWebDavRecovery(tabId, candidate);
});

chrome.tabs.onRemoved.addListener(tabId => {
  chrome.storage.session.remove([sessionStorageKey(tabId), checkedTabKey(tabId)]).catch(() => { });
});

async function maybePromptWebDavRecovery(tabId, candidate) {
  try {
    const settings = await loadWebDavSettings();
    if (!isWebDavReady(settings) || !settings.autoPromptRecovery) return;
    if (!(await hasWebDavOriginPermission(settings.baseUrl))) return;

    // 按标签页 + 当前账号/Token 指纹去重，避免 relay 定时轮询重复请求 WebDAV
    const fingerprint = `${candidate.accountId}|${candidate.token.length}|${candidate.token.slice(0, 12)}`;
    const prev = (await chrome.storage.session.get(checkedTabKey(tabId)))[checkedTabKey(tabId)];
    if (prev && prev.fingerprint === fingerprint && Date.now() - prev.checkedAt < 60_000) return;
    await chrome.storage.session.set({
      [checkedTabKey(tabId)]: { fingerprint, checkedAt: Date.now() }
    });

    const client = new WebDavClient(settings);
    const accounts = await client.listAccounts();
    if (accounts.length === 0) {
      await setBadge(tabId, "");
      return;
    }

    // 情况 1：当前大地图网页未登录（Token 为空）
    if (!candidate.token || candidate.token.length <= 8) {
      await setBadge(tabId, "!");
      await chrome.tabs.sendMessage(tabId, {
        type: "showRecoveryBanner",
        reason: "loggedOut",
        suggestedAccountId: accounts[0].accountId,
        accounts
      }).catch(() => { });
      return;
    }

    // 情况 2：当前网页已登录某账号，检查云端同账号 Token 是否已更新（例如被手机端顶掉线）
    if (isAccountId(candidate.accountId)) {
      const matched = accounts.find(a => a.accountId === candidate.accountId);
      if (matched) {
        const downloaded = await client.download(matched.accountId, settings.passphrase);
        if (downloaded && downloaded.payload.token !== candidate.token) {
          await setBadge(tabId, "!");
          await chrome.tabs.sendMessage(tabId, {
            type: "showRecoveryBanner",
            reason: "updated",
            suggestedAccountId: matched.accountId,
            accounts
          }).catch(() => { });
          return;
        }
      }
    }

    await setBadge(tabId, "");
  } catch {
    // 自动检测失败时静默忽略，绝不干扰正常使用大地图
  }
}

async function getActiveMapTab(explicitTabId) {
  if (typeof explicitTabId === "number") {
    const tab = await chrome.tabs.get(explicitTabId);
    if (tab?.id && MAP_URL_REGEX.test(tab.url || "")) return tab;
  }
  const tab = (await chrome.tabs.query({ active: true, currentWindow: true }))[0];
  if (!tab?.id || !MAP_URL_REGEX.test(tab.url || "")) {
    throw new Error("请先打开库街区鸣潮大地图页面（kurobbs.com/mc/map）。");
  }
  return tab;
}

/**
 * 通过 chrome.scripting.executeScript 直接在大地图页面的 MAIN 世界写入新凭证，
 * 避免依赖 ISOLATED/MAIN 跨世界事件，也不受旧标签页未刷新 content script 的限制。
 */
async function applyAccountToTab(tabId, accountId) {
  const settings = await loadWebDavSettings();
  if (!isWebDavReady(settings)) throw new Error("请先在扩展设置页开启并填好 WebDAV 配置。");
  if (!(await hasWebDavOriginPermission(settings.baseUrl))) {
    throw new Error("尚未授予该 WebDAV 域名的访问权限，请到扩展设置页重新点「保存并授权」。");
  }
  if (!isAccountId(accountId)) throw new Error("无效的库街区账号 ID。");

  const client = new WebDavClient(settings);
  const downloaded = await client.download(accountId, settings.passphrase);
  if (!downloaded) throw new Error(`WebDAV 中未找到账号 ${accountId} 的凭证。`);

  const results = await chrome.scripting.executeScript({
    target: { tabId },
    world: "MAIN",
    args: [downloaded.payload.accountId, downloaded.payload.token, downloaded.payload.userInfoJson || "", downloaded.envelope.displayName || ""],
    func: async (newAccountId, newToken, newUserInfoJson, displayName) => {
      // 写回前先向库街区接口轻量验活，避免把已被退出登录吊销或过期的 Token 写进去后被页面静默清空
      try {
        const devcode = localStorage.getItem("map_dc") || "imao-webdav-check";
        const checkResp = await fetch("https://api.kurobbs.com/map/core/position/getHaveDonePositionIds", {
          method: "POST",
          headers: {
            token: newToken,
            source: "h5",
            wiki_type: "120",
            devcode,
            state_id: "8",
            "Content-Type": "application/x-www-form-urlencoded"
          },
          body: ""
        });
        const checkJson = await checkResp.json();
        if (checkJson?.code !== 200) {
          return {
            verified: false,
            expired: true,
            code: checkJson?.code,
            msg: checkJson?.msg || "登录已失效"
          };
        }
      } catch {
        // 网络或跨域异常时不阻断写回，交由刷新后页面自行处理
      }

      // 构造完整的 profile 对象（确保包含 userId 与最新 token），
      // 因为库街区大地图的 Pinia useUserStore 启动与 UI 登录态完全依赖 AKI_MAP_USER_PROFILE
      let profileObj = {};
      if (newUserInfoJson && newUserInfoJson.trim()) {
        try {
          const parsed = JSON.parse(newUserInfoJson);
          if (parsed && typeof parsed === "object") profileObj = parsed;
        } catch { }
      }
      profileObj.userId = profileObj.userId ? String(profileObj.userId) : String(newAccountId);
      profileObj.token = newToken;
      if (displayName && !profileObj.userName) profileObj.userName = displayName;

      const userInfoSerialized = JSON.stringify(profileObj);
      const piniaPersisted = JSON.stringify({
        profile: profileObj,
        token: newToken
      });

      // 同步写入全部 3 个库街区大地图认证存储键
      localStorage.setItem("AKI_MAP_USER_TOKEN", newToken);
      localStorage.setItem("AKI_MAP_USER_INFO", userInfoSerialized);
      localStorage.setItem("AKI_MAP_USER_PROFILE", piniaPersisted);

      const verified =
        localStorage.getItem("AKI_MAP_USER_TOKEN") === newToken &&
        localStorage.getItem("AKI_MAP_USER_PROFILE") === piniaPersisted;
      return { verified, accountId: newAccountId };
    }
  });

  const execResult = results?.[0]?.result;
  if (execResult?.expired) {
    throw new Error(`账号 ${accountId} 的云端凭证已在库街区失效（code=${execResult.code}，${execResult.msg}），可能曾在网页点过退出登录或已过期，请重新登录并上传新凭证。`);
  }
  if (!execResult?.verified) {
    throw new Error("未能将新凭证写入大地图页面存储，请刷新页面后重试。");
  }

  // 清除缓存的旧指纹并由浏览器原生刷新标签页，让大地图以新凭证重新加载
  await chrome.storage.session.remove([sessionStorageKey(tabId), checkedTabKey(tabId)]).catch(() => { });
  await setBadge(tabId, "");
  await chrome.tabs.reload(tabId);
  return {
    accountId: downloaded.payload.accountId,
    version: downloaded.envelope.version,
    sourceDevice: downloaded.envelope.sourceDevice
  };
}

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (!message?.type) return false;

  // 1. 原有核心能力：连接本机桌面端 IMao（自动识别账号 ID，WebDAV 关闭时与原流程完全一致）
  if (message.type === "connect") {
    (async () => {
      const tab = await getActiveMapTab();
      const session = await readLiveSessionFromTab(tab.id);
      const profile = profileId(session?.accountId || "");
      if (!profile || !session?.token) {
        const seen = session ? `已读取页面会话但缺少账号信息（${session.diagnostic || "无诊断信息"}）` : "未检测到已登录的库街区会话";
        throw new Error(`未找到可用登录会话：${seen}。请确认已在库街区大地图登录，刷新页面后重试。`);
      }
      const response = await chrome.runtime.sendNativeMessage(HOST_NAME, {
        version: 1,
        type: "storeCredential",
        profileId: profile,
        token: session.token,
        accountId: String(session.accountId || "")
      });
      if (!response?.accepted) throw new Error("桌面端拒绝保存凭据：" + (response?.error || "unknown"));
      await chrome.storage.session.remove(sessionStorageKey(tab.id));

      const accountId = response.profileId.replace(/^kuro_/, "");
      let webdavUploaded = false;
      let webdavVersion = 0;
      let webdavError = "";

      // 若玩家已开启并授权了 WebDAV，连接桌面端成功后顺手加密上传一份（上传失败不影响桌面端连接结果）
      try {
        const settings = await loadWebDavSettings();
        if (isWebDavReady(settings) && isAccountId(accountId) && await hasWebDavOriginPermission(settings.baseUrl)) {
          const client = new WebDavClient(settings);
          const envelope = await client.upload({
            accountId,
            token: session.token,
            passphrase: settings.passphrase,
            sourceDevice: "browser",
            displayName: session.displayName || "",
            userInfoJson: session.userInfoJson || ""
          });
          webdavUploaded = true;
          webdavVersion = envelope.version;
          await setBadge(tab.id, "");
        }
      } catch (err) {
        webdavError = err?.message || String(err);
      }

      return { profileId: response.profileId, accountId, webdavUploaded, webdavVersion, webdavError };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 2. 读取 WebDAV 配置（供 popup 和 options 页使用）
  if (message.type === "webdavGetSettings") {
    (async () => {
      const settings = await loadWebDavSettings();
      const permissionGranted = settings.baseUrl ? await hasWebDavOriginPermission(settings.baseUrl) : false;
      return { settings, ready: isWebDavReady(settings), permissionGranted };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 3. 保存 WebDAV 配置
  if (message.type === "webdavSaveSettings") {
    (async () => {
      const next = { ...DEFAULT_WEBDAV_SETTINGS, ...(message.settings || {}) };
      if (next.enabled || next.baseUrl.trim()) {
        next.baseUrl = validateBaseUrl(next.baseUrl);
      } else {
        next.baseUrl = "";
      }
      next.username = String(next.username || "").trim();
      next.password = String(next.password || "");
      next.passphrase = String(next.passphrase || "");
      next.autoPromptRecovery = Boolean(next.autoPromptRecovery);
      await chrome.storage.local.set({ [WEBDAV_SETTINGS_KEY]: next });
      return { settings: next, ready: isWebDavReady(next) };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 4. 测试 WebDAV 连接并列出账号
  if (message.type === "webdavTestConnection") {
    (async () => {
      const draft = message.settings ? { ...DEFAULT_WEBDAV_SETTINGS, ...message.settings } : await loadWebDavSettings();
      if (!draft.passphrase) throw new Error("请输入同步加密口令。");
      if (!(await hasWebDavOriginPermission(draft.baseUrl))) {
        throw new Error("尚未授予该 WebDAV 域名的访问权限，请先点击「保存并授权」。");
      }
      const client = new WebDavClient(draft);
      const accounts = await client.testConnection();
      return { accounts };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 5. 列出 WebDAV 云端所有账号
  if (message.type === "webdavListAccounts") {
    (async () => {
      const settings = await loadWebDavSettings();
      if (!isWebDavReady(settings)) return { accounts: [], ready: false };
      if (!(await hasWebDavOriginPermission(settings.baseUrl))) {
        throw new Error("尚未授予该 WebDAV 域名的访问权限，请到扩展设置页重新授权。");
      }
      const client = new WebDavClient(settings);
      const accounts = await client.listAccounts();
      return { accounts, ready: true };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 6. 上传当前大地图网页的登录凭证到 WebDAV（全程自动识别账号 ID）
  if (message.type === "webdavUploadCurrentTab") {
    (async () => {
      const settings = await loadWebDavSettings();
      if (!isWebDavReady(settings)) throw new Error("请先在扩展设置页开启并填好 WebDAV 配置。");
      if (!(await hasWebDavOriginPermission(settings.baseUrl))) {
        throw new Error("尚未授予该 WebDAV 域名的访问权限，请到扩展设置页重新点「保存并授权」。");
      }
      const tab = await getActiveMapTab();
      const session = await readLiveSessionFromTab(tab.id);
      const accountId = isAccountId(session?.accountId) ? session.accountId : "";
      if (!accountId || !session?.token) {
        throw new Error("当前大地图网页未检测到已登录的库街区账号，请先登录后刷新页面再试。");
      }
      const client = new WebDavClient(settings);
      const envelope = await client.upload({
        accountId,
        token: session.token,
        passphrase: settings.passphrase,
        sourceDevice: "browser",
        displayName: session.displayName || "",
        userInfoJson: session.userInfoJson || ""
      });
      await setBadge(tab.id, "");
      return {
        accountId: envelope.accountId,
        version: envelope.version,
        updatedAtUtc: envelope.updatedAtUtc
      };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 7. 将 WebDAV 指定账号凭证恢复/切换到当前大地图网页
  if (message.type === "webdavApplyToCurrentTab") {
    (async () => {
      const tab = await getActiveMapTab(sender?.tab?.id);
      return await applyAccountToTab(tab.id, String(message.accountId || ""));
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 8. 删除 WebDAV 上指定账号的云端凭证
  if (message.type === "webdavDeleteAccount") {
    (async () => {
      const settings = await loadWebDavSettings();
      if (!isWebDavReady(settings)) throw new Error("WebDAV 尚未启用或配置不完整。");
      const client = new WebDavClient(settings);
      await client.deleteAccount(String(message.accountId || ""));
      return { ok: true };
    })().then(sendResponse, error => sendResponse({ error: error.message }));
    return true;
  }

  // 9. 编解码便携配置文本
  if (message.type === "webdavEncodeBundle") {
    try {
      sendResponse({ text: encodeConfigBundle(message.bundle || {}) });
    } catch (error) {
      sendResponse({ error: error.message });
    }
    return false;
  }

  if (message.type === "webdavDecodeBundle") {
    try {
      sendResponse({ bundle: decodeConfigBundle(message.text) });
    } catch (error) {
      sendResponse({ error: error.message });
    }
    return false;
  }

  return false;
});
