const status = document.querySelector("#status");
document.querySelector("#connect").addEventListener("click", async () => {
  status.textContent = "正在连接…";
  const response = await chrome.runtime.sendMessage({ type: "connect", profileId: document.querySelector("#profile").value.trim() });
  status.textContent = response?.error ? `连接失败：${response.error}` : `已连接到同步档案 ${response.profileId}。`;
});
