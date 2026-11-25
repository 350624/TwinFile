"use strict";
const box = document.getElementById("takeover");
const status = document.getElementById("status");
chrome.storage.local.get({takeover: false, bridgeError: false}).then(data => {
  box.checked = !!data.takeover; box.disabled = false;
  if (data.bridgeError) status.textContent = "未能启动下载器。请确认已注册 Rust 原生桥接。";
}).catch(() => { status.textContent = "无法读取扩展设置。"; });
box.addEventListener("change", async () => {
  const requested = box.checked;
  box.disabled = true;
  try { await chrome.storage.local.set({takeover: requested}); }
  catch { box.checked = !requested; status.textContent = "无法保存扩展设置。"; }
  finally { box.disabled = false; }
});
