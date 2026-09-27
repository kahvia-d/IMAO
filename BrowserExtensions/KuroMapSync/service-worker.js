const host = "com.imao.kuro_sync";
const storageKey = tabId => `candidate:${tabId}`;

chrome.runtime.onMessage.addListener((message, sender) => {
  if (message?.type !== "sessionCandidate" || sender.tab?.id == null || !/^https:\/\/(www\.)?kurobbs\.com\/mc\/map\//.test(sender.tab.url || "")) return;
  if (typeof message.token !== "string" || message.token.length > 16384) return;
  chrome.storage.session.set({
    [storageKey(sender.tab.id)]: { accountId: message.accountId, token: message.token, diagnostic: String(message.diagnostic || "") }
  });
});

async function sessionFor(tabId) {
  const cached = (await chrome.storage.session.get(storageKey(tabId)))[storageKey(tabId)];
  return cached?.token?.length > 8 ? cached : null;
}

// A credential's file name on the desktop is derived from the Kuro account it belongs to, and
// this prefix is half of that one rule (the other half is KuroTokenVault.AccountCredentialId).
// A value that already carries the prefix is never prefixed twice: the popup shows players
// "kuro_<id>", and pasting that back has to mean the same account.
function profileId(value) {
  const normalized = String(value || "").replace(/[^a-zA-Z0-9_-]/g, "").replace(/^kuro_/, "").slice(0, 80);
  return normalized ? `kuro_${normalized}` : "";
}

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message?.type !== "connect") return;
  (async () => {
    const tab = (await chrome.tabs.query({ active: true, currentWindow: true }))[0];
    if (!tab?.id || !/^https:\/\/(www\.)?kurobbs\.com\/mc\/map\//.test(tab.url || "")) throw new Error("请先打开已登录的库街区鸣潮大地图。");
    const session = await sessionFor(tab.id);
    // The account the page reported wins; what the player typed is only a fallback for a page
    // that does not expose it.
    const typed = String(message.profileId || "").trim();
    const profile = profileId(session?.accountId || typed);
    if (!profile || !session?.token) {
      const cached = (await chrome.storage.session.get(storageKey(tab.id)))[storageKey(tab.id)];
      const seen = cached ? `已收到页面会话但不可用（${cached.diagnostic || "无诊断信息"}）` : "页面脚本未上报会话";
      throw new Error(`未找到可用登录会话：${seen}。请确认已登录，并在刷新地图页后重试。`);
    }
    const response = await chrome.runtime.sendNativeMessage(host, { version: 1, type: "storeCredential", profileId: profile, token: session.token, accountId: String(session.accountId || "") });
    if (!response?.accepted) throw new Error("桌面端拒绝保存凭据：" + (response?.error || "unknown"));
    await chrome.storage.session.remove(storageKey(tab.id));
    // The bare account id is what the player has to type into the record book they want to sync.
    return { profileId: response.profileId, accountId: response.profileId.replace(/^kuro_/, "") };
  })().then(sendResponse, error => sendResponse({ error: error.message }));
  return true;
});
