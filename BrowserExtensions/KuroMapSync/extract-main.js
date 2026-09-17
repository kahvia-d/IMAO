(() => {
  const readSession = () => {
    const token = localStorage.getItem("AKI_MAP_USER_TOKEN") || "";
    let accountId = "";
    let keys = "";
    try {
      const info = JSON.parse(localStorage.getItem("AKI_MAP_USER_INFO") || "{}");
      accountId = String(info?.userId ?? "");
      keys = Object.keys(info || {}).slice(0, 12).join(",");
    } catch { }
    // Only non-secret facts are reported; the popup shows them when connecting fails.
    return { accountId, token, diagnostic: `tokenLength=${token.length} userIdLength=${accountId.length} userInfoKeys=${keys}` };
  };
  const publish = () => window.dispatchEvent(new CustomEvent("imao-kuro-session", { detail: readSession() }));
  // This script runs before the isolated-world relay attaches its listener, so
  // the relay can ask for the session again instead of relying on one event.
  window.addEventListener("imao-kuro-session-request", publish);
  publish();
})();
