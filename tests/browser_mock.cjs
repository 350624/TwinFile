const fs = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");
const src = fs.readFileSync("browser-extension/background.js", "utf8");
async function scenario(settings) {
  const calls = [];
  const event = () => ({ handler: null, addListener(f) { this.handler = f; } });
  const downloads = {
    onCreated: event(),
    async pause() { calls.push("pause"); if (settings.pauseFails) throw Error("gone"); },
    async resume() { calls.push("resume"); },
    async cancel() { calls.push("cancel"); if (settings.cancelFails) throw Error("gone"); },
  };
  const runtime = {onInstalled: event(), async sendNativeMessage(host, payload) {
    assert.equal(host, "com.twinfile.rust.downloader");
    assert.deepEqual(Object.keys(payload).sort(), ["filename", "url"]);
    calls.push("native");
    if (settings.reject) throw Error("host unavailable");
    return {accepted: settings.accepted};
  }};
  const chrome = {runtime, downloads, contextMenus: {onClicked: event()}, action: {async setBadgeText() {}},
    storage: {local: {async get() {return {takeover: settings.enabled};}, async set() {}}}};
  vm.runInNewContext(src, {chrome, URL});
  await downloads.onCreated.handler({id: 1, url: settings.url || "https://example.test/a.zip", filename: "", state: "in_progress"});
  return calls;
}
(async () => {
  assert.deepEqual(await scenario({enabled: false}), []);
  assert.deepEqual(await scenario({enabled: true, accepted: true}), ["pause", "native", "cancel"]);
  assert.deepEqual(await scenario({enabled: true, reject: true}), ["pause", "native", "resume"]);
  assert.deepEqual(await scenario({enabled: true, accepted: false}), ["pause", "native", "resume"]);
  assert.deepEqual(await scenario({enabled: true, accepted: true, cancelFails: true}), ["pause", "native", "cancel", "resume"]);
  assert.deepEqual(await scenario({enabled: true, pauseFails: true}), ["pause"]);
  assert.deepEqual(await scenario({enabled: true, url: "file:///C:/file.zip"}), []);
  console.log("7 browser bridge scenarios passed");
})().catch(e => {console.error(e); process.exitCode=1;});
