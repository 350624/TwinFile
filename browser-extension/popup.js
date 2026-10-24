const toggle = document.querySelector("#takeover");
chrome.storage.local.get({ takeover: false }, settings => { toggle.checked = settings.takeover; });
toggle.addEventListener("change", () => chrome.storage.local.set({ takeover: toggle.checked }));
chrome.storage.local.get({ bridgeError: "" }, settings => {
  if (settings.bridgeError) document.querySelector("#status").textContent = `下载器连接失败：${settings.bridgeError}`;
});
