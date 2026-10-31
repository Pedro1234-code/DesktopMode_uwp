"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const engineRoot = process.argv[2];
assert.ok(engineRoot, "Pass the Firefox engine source directory as argument 1");
const addonRoot = path.join(engineRoot, "browser/extensions/webcompat");
const scriptPath = "injections/js/gecko-w10m-parsec-clipboard.js";
const source = fs.readFileSync(path.join(addonRoot, scriptPath), "utf8");
const manifest = JSON.parse(fs.readFileSync(path.join(addonRoot, "manifest.json"), "utf8"));
const registration = manifest.content_scripts.find(entry => entry.js.includes(scriptPath));
assert.deepEqual(registration.matches, ["https://web.parsec.app/*"]);
assert.equal(registration.run_at, "document_start");
assert.equal(registration.world, "MAIN");
assert.equal(registration.all_frames, true);
assert.ok(!manifest.permissions.some(permission => permission.startsWith("clipboard")));

function install(clipboard, origin = "https://web.parsec.app") {
  const logs = [];
  vm.runInNewContext(source, {
    location: { origin },
    navigator: { clipboard },
    console: { info: message => logs.push(message) },
  });
  return logs;
}

async function main() {
  let calls = 0;
  const denial = Object.assign(new Error("Permission denied"), { name: "NotAllowedError" });
  const writeText = async () => {};
  const clipboard = {
    async readText() {
      assert.equal(this, clipboard);
      calls++;
      throw denial;
    },
    writeText,
  };
  let signaled = false;
  const workerReply = { size: -1 };
  async function matoyaGetClipboard() {
    const text = await clipboard.readText();
    workerReply.size = Buffer.byteLength(text);
    signaled = true;
  }
  await assert.rejects(matoyaGetClipboard(), error => error === denial);
  assert.equal(signaled, false, "Without the shim, rejection skips the worker signal");
  calls = 0;
  const logs = install(clipboard);
  await matoyaGetClipboard();
  assert.equal(signaled, true);
  assert.equal(workerReply.size, 0);
  assert.equal(await clipboard.readText(), "");
  assert.equal(calls, 2, "Exactly one native read per request; no permission retries");
  assert.equal(logs.length, 1, "Log denial once per document");
  assert.ok(!logs[0].includes(denial.message));
  assert.equal(clipboard.writeText, writeText);

  for (const text of ["test text", "", "á😀"]) {
    const allowed = { async readText() { return text; } };
    const allowedLogs = install(allowed);
    assert.equal(await allowed.readText(), text);
    assert.equal(allowedLogs.length, 0);
  }

  const failure = new Error("Other failure");
  const broken = { async readText() { throw failure; } };
  install(broken);
  await assert.rejects(broken.readText(), error => error === failure);

  const receiverChecked = { async readText() { throw denial; } };
  install(receiverChecked);
  await assert.rejects(receiverChecked.readText.call({}), error => error === denial);

  const syncDenied = { readText() { throw denial; } };
  install(syncDenied);
  assert.equal(await syncDenied.readText(), "");

  for (const origin of ["https://example.com", "https://parsec.app", "http://web.parsec.app", "https://web.parsec.app:11443", "null"]) {
    const other = { async readText() { throw denial; } };
    const original = other.readText;
    assert.equal(install(other, origin).length, 0);
    assert.equal(other.readText, original);
    await assert.rejects(other.readText(), error => error === denial);
  }
  assert.deepEqual(install(undefined), []);
  const missingRead = {};
  assert.deepEqual(install(missingRead), []);
  assert.equal(Object.hasOwn(missingRead, "readText"), false);
  console.log("Parsec clipboard compatibility checks passed (mocked APIs; no browser/Xbox validation).");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
