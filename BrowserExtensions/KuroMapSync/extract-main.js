(() => {
  // 在页面主世界读取与写回库街区大地图的 localStorage 登录会话（含 Pinia 持久化的 AKI_MAP_USER_PROFILE）
  const readSession = () => {
    let token = localStorage.getItem("AKI_MAP_USER_TOKEN") || "";
    let userInfoRaw = localStorage.getItem("AKI_MAP_USER_INFO") || "";
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
        const parsed = JSON.parse(userInfoRaw || "{}");
        if (parsed && typeof parsed === "object" && Object.keys(parsed).length > 0) profileObj = parsed;
      } catch { }
    }
    if (!userInfoRaw && profileObj) {
      try { userInfoRaw = JSON.stringify(profileObj); } catch { }
    }
    const accountId = String(profileObj?.userId ?? "");
    const displayName = String(profileObj?.userName ?? profileObj?.nickName ?? profileObj?.name ?? "");
    const keys = profileObj ? Object.keys(profileObj).slice(0, 12).join(",") : "";
    // 诊断字段不含任何敏感凭据，仅用于连接失败时定位原因
    return {
      accountId,
      displayName,
      token,
      userInfoJson: userInfoRaw,
      diagnostic: `tokenLength=${token.length} userIdLength=${accountId.length} userInfoKeys=${keys}`
    };
  };

  const publish = () => window.dispatchEvent(new CustomEvent("imao-kuro-session", { detail: readSession() }));

  // 接收隔离世界中继传来的“恢复/切换登录”请求，写回 localStorage 并刷新大地图页面
  window.addEventListener("imao-kuro-apply-session", event => {
    const detail = event?.detail;
    if (!detail || typeof detail.token !== "string" || !detail.token.trim()) return;
    try {
      localStorage.setItem("AKI_MAP_USER_TOKEN", detail.token);
      if (typeof detail.userInfoJson === "string" && detail.userInfoJson.trim()) {
        localStorage.setItem("AKI_MAP_USER_INFO", detail.userInfoJson);
      } else if (detail.accountId) {
        // 若云端未带 userInfoJson 且本地旧缓存属于不同账号，先清掉旧缓存让页面按新 Token 拉取
        try {
          const existing = JSON.parse(localStorage.getItem("AKI_MAP_USER_INFO") || "{}");
          if (existing?.userId && String(existing.userId) !== String(detail.accountId)) {
            localStorage.removeItem("AKI_MAP_USER_INFO");
          }
        } catch {
          localStorage.removeItem("AKI_MAP_USER_INFO");
        }
      }
      window.dispatchEvent(new CustomEvent("imao-kuro-apply-result", { detail: { ok: true } }));
      window.location.reload();
    } catch (error) {
      window.dispatchEvent(new CustomEvent("imao-kuro-apply-result", {
        detail: { ok: false, error: String(error?.message || error) }
      }));
    }
  });

  // 此脚本可能早于隔离世界监听器执行，支持按需重复上报
  window.addEventListener("imao-kuro-session-request", publish);
  publish();
})();
