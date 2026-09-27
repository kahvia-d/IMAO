const status = document.querySelector("#status");
const profile = document.querySelector("#profile");
document.querySelector("#connect").addEventListener("click", async () => {
  status.textContent = "正在连接…";
  const response = await chrome.runtime.sendMessage({ type: "connect", profileId: profile.value.trim() });
  status.textContent = response?.error
    ? `连接失败：${response.error}`
    : `已连接库街区账号 ${response.accountId}。\n\n回到 IMao → 设置 → 本地点位记录本 → 点「修改」，把 ${response.accountId} 填进「绑定库街区账号」。`;
});
