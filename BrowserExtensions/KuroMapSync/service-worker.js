const host = "com.imao.kuro_sync";
const storageKey = tabId => `candidate:${tabId}`;

chrome.runtime.onMessage.addListener((message, sender) => {
  if (message?.type !== "sessionCandidate" || sender.tab?.id == null || !/^https:\/\/(www\.)?kurobbs\.com\/mc\/map\//.test(sender.tab.url || "")) return;
  if (typeof message.token !== "string" || message.token.length > 16384) return;
  chrome.storage.session.set({ [storageKey(sender.tab.id)]: { accountId: message.accountId, token: message.token } });
});

async function sessionFor(tabId) {
  const cached = (await chrome.storage.session.get(storageKey(tabId)))[storageKey(tabId)];
  return cached?.token?.length > 8 ? cached : null;
}

function profileId(accountId) {
  const normalized = accountId.replace(/[^a-zA-Z0-9_-]/g, "").slice(0, 80);
  return normalized ? `kuro_${normalized}` : "";
}

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message?.type !== "connect") return;
  (async () => {
    const tab = (await chrome.tabs.query({ active: true, currentWindow: true }))[0];
    if (!tab?.id || !/^https:\/\/(www\.)?kurobbs\.com\/mc\/map\//.test(tab.url || "")) throw new Error("请先打开已登录的库街区鸣潮大地图。");
    const session = await sessionFor(tab.id);
    const profile = profileId(message.profileId || session?.accountId || "");
    if (!profile || !session?.token) throw new Error("未找到可用登录会话。请确认已登录，并在地图页刷新后重试。");
    const response = await chrome.runtime.sendNativeMessage(host, { version: 1, type: "storeCredential", profileId: profile, token: session.token });
    if (!response?.accepted) throw new Error("桌面端拒绝保存凭据：" + (response?.error || "unknown"));
    await chrome.storage.session.remove(storageKey(tab.id));
    return { profileId: response.profileId };
  })().then(sendResponse, error => sendResponse({ error: error.message }));
  return true;
});
