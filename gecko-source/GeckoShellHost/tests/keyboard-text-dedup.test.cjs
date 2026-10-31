"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const source = fs.readFileSync(path.join(__dirname, "../client/EngineView.cpp"), "utf8");
const keyboard = source.slice(source.indexOf("void EngineView::WireKeyboard()"), source.indexOf("void EngineView::ArmBridge()"));
assert.ok(keyboard.includes("sink_.TextChanging("));
assert.ok(keyboard.includes("Controls::TextBoxTextChangingEventArgs const&"));
assert.ok(!keyboard.includes("sink_.TextChanged("));
assert.ok(keyboard.includes("textRevision_ == hardwareTextRevision_"));

function transport() {
  return {
    revision: 0,
    keyRevision: 0,
    pending: false,
    output: "",
    keyDown() {
      this.pending = true;
      this.keyRevision = this.revision;
    },
    textChanging(text, clearing = false) {
      if (clearing || !text) return;
      this.output += text;
      this.revision++;
      this.textChanging("", true);
    },
    characterReceived(text) {
      if (!this.pending) return false;
      this.pending = false;
      if (this.revision === this.keyRevision) {
        this.output += text;
        this.revision++;
      }
      return true;
    },
  };
}

for (const text of ["d", "dd", "hello", ".,:", "aaa...", "áéç"]) {
  for (const xamlFirst of [true, false]) {
    const state = transport();
    for (const character of text) {
      state.keyDown();
      if (xamlFirst) state.textChanging(character);
      const handled = state.characterReceived(character);
      if (!xamlFirst && !handled) state.textChanging(character);
    }
    assert.equal(state.output, text);
  }
  const virtualKeyboard = transport();
  virtualKeyboard.textChanging(text);
  assert.equal(virtualKeyboard.output, text);
}

const old = transport();
old.keyDown();
old.characterReceived("d");
old.textChanging("d");
assert.equal(old.output, "dd", "A deferred TextChanged reproduces the old duplicate");

console.log("Keyboard source and event-order model checks passed; real XAML/Xbox delivery not exercised.");
