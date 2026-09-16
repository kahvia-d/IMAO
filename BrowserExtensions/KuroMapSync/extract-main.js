(() => {
  const token = localStorage.getItem("AKI_MAP_USER_TOKEN") || "";
  let accountId = "";
  try { accountId = String(JSON.parse(localStorage.getItem("AKI_MAP_USER_INFO") || "{}").userId || ""); } catch { }
  window.dispatchEvent(new CustomEvent("imao-kuro-session", { detail: { accountId, token } }));
})();
