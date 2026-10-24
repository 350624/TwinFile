const hostName = "com.twinfile.downloader";
const pending = new Set();

function sendToDownloader(url, filename, callback) {
  const clearStatus = () => {
    chrome.action.setBadgeText({ text: "" });
    chrome.storage.local.remove("bridgeError");
  };
  chrome.runtime.sendNativeMessage(hostName, { url, filename }, response => {
    const accepted = !chrome.runtime.lastError && response?.accepted === true;
    if (!accepted) {
      const message = chrome.runtime.lastError?.message || "下载器未能启动";
      chrome.action.setBadgeText({ text: "!" });
      chrome.action.setBadgeBackgroundColor({ color: "#b42318" });
      chrome.storage.local.set({ bridgeError: message });
    } else {
      clearStatus();
    }
    callback(accepted);
  });
}

chrome.downloads.onCreated.addListener(item => {
  if (item.byExtensionId || !item.url || !/^https?:/i.test(item.url) || pending.has(item.id)) return;
  pending.add(item.id);
  chrome.storage.local.get({ takeover: false }, settings => {
    if (!settings.takeover) { pending.delete(item.id); return; }
    chrome.downloads.pause(item.id, () => {
      if (chrome.runtime.lastError) { pending.delete(item.id); return; }
      const filename = item.filename.split(/[\\/]/).pop() || new URL(item.url).pathname.split("/").pop() || "download.bin";
      sendToDownloader(item.url, filename, accepted => {
        if (accepted) chrome.downloads.cancel(item.id, () => void chrome.runtime.lastError);
        else chrome.downloads.resume(item.id, () => void chrome.runtime.lastError);
        pending.delete(item.id);
      });
    });
  });
});

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({ id: "twinfile-download-link", title: "用 TwinFile Downloader 下载链接", contexts: ["link"] });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId !== "twinfile-download-link" || !info.linkUrl) return;
  let filename = "download.bin";
  try { filename = new URL(info.linkUrl).pathname.split("/").pop() || filename; } catch {}
  sendToDownloader(info.linkUrl, filename, () => {});
});
