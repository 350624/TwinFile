"use strict";
const host = "com.twinfile.rust.downloader";
const menuId = "twinfile-rust-link";
const pending = new Set();
function httpUrl(url) {
  try {
    const u = new URL(url);
    return (u.protocol === "http:" || u.protocol === "https:") && !u.username && !u.password && !u.hash ? u : null;
  } catch { return null; }
}
function suggest(u, name) {
  const last = (name || "").split(/[\\/]/).pop();
  if (last) return last;
  try { return decodeURIComponent(u.pathname.split("/").pop()) || "download.bin"; }
  catch { return "download.bin"; }
}
async function send(u, name) {
  let accepted = false;
  try { accepted = (await chrome.runtime.sendNativeMessage(host, {url: u.href, filename: suggest(u, name)}))?.accepted === true; }
  catch { /* Opening the original task remains the browser's responsibility. */ }
  await Promise.allSettled([
    chrome.action.setBadgeText({text: accepted ? "" : "!"}),
    chrome.storage.local.set({bridgeError: !accepted})
  ]);
  return accepted;
}
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({id: menuId, title: "用 TwinFile Rust 下载链接", contexts: ["link"]}, () => void chrome.runtime.lastError);
});
chrome.contextMenus.onClicked.addListener((item) => {
  const u = httpUrl(item.linkUrl);
  if (item.menuItemId === menuId && u) void send(u);
});
chrome.downloads.onCreated.addListener(async item => {
  const u = httpUrl(item.url);
  if (!u || item.byExtensionId || item.state !== "in_progress" || pending.has(item.id)) return;
  pending.add(item.id);
  let paused = false;
  try {
    if (!(await chrome.storage.local.get({takeover: false})).takeover) return;
    await chrome.downloads.pause(item.id);
    paused = true;
    if (await send(u, item.filename)) {
      await chrome.downloads.cancel(item.id);
      paused = false;
    }
  } catch { /* If any operation fails, do not leave the browser task paused. */ }
  finally {
    if (paused) { try { await chrome.downloads.resume(item.id); } catch {} }
    pending.delete(item.id);
  }
});
