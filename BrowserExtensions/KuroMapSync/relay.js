window.addEventListener("imao-kuro-session", event => {
  const value = event.detail;
  if (!value || typeof value.token !== "string") return;
  chrome.runtime.sendMessage({ type: "sessionCandidate", accountId: String(value.accountId || ""), token: value.token });
});
