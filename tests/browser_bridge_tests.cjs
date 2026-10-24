const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const source = fs.readFileSync(new URL("../browser-extension/background.js", `file://${__filename}`), "utf8");

function createBridge({ takeover = false, nativeAccepted = true, extensionId = undefined } = {}) {
  const listeners = {};
  const state = { takeover, nativeAccepted, extensionId, calls: [], badge: "", stored: {} };
  const event = name => ({ addListener(callback) { listeners[name] = callback; } });
  const chrome = {
    runtime: {
      onInstalled: event("installed"),
      lastError: null,
      sendNativeMessage(host, message, callback) {
        state.calls.push({ type: "native", host, message });
        chrome.runtime.lastError = state.nativeAccepted ? null : { message: "host unavailable" };
        callback(state.nativeAccepted ? { accepted: true } : undefined);
        chrome.runtime.lastError = null;
      }
    },
    downloads: {
      onCreated: event("download"),
      pause(id, callback) { state.calls.push({ type: "pause", id }); callback(); },
      resume(id, callback) { state.calls.push({ type: "resume", id }); callback(); },
      cancel(id, callback) { state.calls.push({ type: "cancel", id }); callback(); }
    },
    storage: { local: {
      get(defaults, callback) { callback({ ...defaults, takeover: state.takeover, ...state.stored }); },
      set(values) { Object.assign(state.stored, values); },
      remove(key) { delete state.stored[key]; }
    } },
    action: {
      setBadgeText({ text }) { state.badge = text; },
      setBadgeBackgroundColor() {}
    },
    contextMenus: {
      create() {}, onClicked: event("contextMenu"),
      onInstalled: event("installed")
    }
  };
  const context = { chrome, URL, console, Set };
  vm.runInNewContext(source, context, { filename: "background.js" });
  return { state, listeners };
}

{
  const { state, listeners } = createBridge();
  listeners.download({ id: 1, url: "https://example.test/a.bin", filename: "a.bin" });
  assert.deepEqual(state.calls, [], "default-off takeover leaves browser download untouched");
}

{
  const { state, listeners } = createBridge({ takeover: true });
  listeners.download({ id: 2, url: "https://example.test/a.bin", filename: "C:\\Downloads\\a.bin" });
  assert.deepEqual(state.calls.map(call => call.type), ["pause", "native", "cancel"]);
  assert.equal(state.calls[1].message.filename, "a.bin");
}

{
  const { state, listeners } = createBridge({ takeover: true, nativeAccepted: false });
  listeners.download({ id: 3, url: "https://example.test/a.bin", filename: "a.bin" });
  assert.deepEqual(state.calls.map(call => call.type), ["pause", "native", "resume"]);
  assert.equal(state.badge, "!");
}

{
  const { state, listeners } = createBridge({ takeover: true, extensionId: "other" });
  listeners.download({ id: 4, byExtensionId: "other", url: "https://example.test/a.bin", filename: "a.bin" });
  assert.deepEqual(state.calls, [], "downloads created by extensions are ignored");
}

{
  const { state, listeners } = createBridge({ nativeAccepted: true });
  listeners.contextMenu({ menuItemId: "twinfile-download-link", linkUrl: "https://example.test/folder/a%20b.zip?token=x" }, {});
  assert.equal(state.calls[0].type, "native");
  assert.equal(state.calls[0].message.url, "https://example.test/folder/a%20b.zip?token=x");
  assert.equal(state.calls[0].message.filename, "a%20b.zip");
}

console.log("Browser bridge regression tests: 5 passed, 0 failed");
