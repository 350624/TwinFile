"use strict";
const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const [packageDir, testRoot] = process.argv.slice(2);
const config = path.join(testRoot, "bridge-config");
const isolated = path.join(testRoot, "isolated-host");
const host = "TwinFileRustDownloadHost.exe";
const systemRoot = process.env.SystemRoot || "C:/Windows";
fs.mkdirSync(config);
fs.mkdirSync(isolated);
fs.copyFileSync(path.join(packageDir, host), path.join(isolated, host));
const env = Object.fromEntries(Object.entries(process.env)
  .filter(([key]) => !["path", "appdata"].includes(key.toLowerCase())));
env.Path = systemRoot + "/System32;" + systemRoot;
env.APPDATA = config;

function request(dir, url) {
  const body = Buffer.from(JSON.stringify({ url, filename: "测试.bin" }));
  const header = Buffer.alloc(4);
  header.writeUInt32LE(body.length);
  const reply = spawnSync(path.join(dir, host), {
    input: Buffer.concat([header, body]),
    windowsHide: true,
    timeout: 18000,
    env
  });
  if (reply.error) throw reply.error;
  assert.equal(reply.status, 0);
  assert.equal(reply.stdout.readUInt32LE(), reply.stdout.length - 4);
  return JSON.parse(reply.stdout.subarray(4).toString("utf8")).accepted;
}

assert.equal(request(packageDir, "invalid-url"), false);
assert.equal(request(isolated, "http://127.0.0.1:9/test.bin"), false);
assert.equal(request(packageDir, "http://127.0.0.1:9/test.bin"), true);
console.log("Native bridge: invalid URL => false; missing downloader => false; downloader UI => true (Windows-only PATH).");
