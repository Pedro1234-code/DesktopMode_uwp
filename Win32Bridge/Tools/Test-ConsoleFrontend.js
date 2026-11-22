// Transport contracts only; no WebView, C++ or C# is built/executed.
'use strict';
const fs = require('fs'), vm = require('vm'), path = require('path'), assert = require('assert');
const messages = [], writes = []; let input, resized, terminal, fits = 0;
class Terminal {
 constructor(options) { terminal=this; this.options=options; this.cols=80; this.rows=25; }
 loadAddon() {} open() {} focus() {} onData(fn) { input=fn; } write(value) { writes.push(value); }
}
const window = { external:{notify(value){messages.push(JSON.parse(value));}}, addEventListener(name,fn){if(name==='resize')resized=fn;} };
const context = { Terminal, FitAddon:{FitAddon:class{fit(){fits++;}}}, window, document:{getElementById(){return {};}} };
vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../../factoryos-10x-shell/Assets/Terminal/terminal.js'),'utf8'), context);
assert(messages.some(x=>x.ready)); assert(messages.some(x=>x.cols===80 && x.rows===25));
messages.length=0; input('ab\x7fc\r'); assert.deepStrictEqual(messages,[{input:'ac\r\n'}]);
assert(writes.includes('\b \b')); messages.length=0;
input('paste\r\n'); assert.deepStrictEqual(messages,[{input:'paste\r\n'}]); messages.length=0;
input('\x1b[A'); input('x\r'); assert.deepStrictEqual(messages,[{input:'x\r\n'}]); messages.length=0;
window.bridgeEnabled('false'); input('ignored\r'); assert.strictEqual(messages.length,0); assert(terminal.options.disableStdin);
window.bridgeEnabled('true'); window.bridgeMode('0'); input('\x1b[A'); assert.deepStrictEqual(messages,[{input:'\x1b[A'}]);
messages.length=0; writes.length=0; window.bridgeMode('2'); input('secret\r'); assert.deepStrictEqual(messages,[{input:'secret\r\n'}]); assert.strictEqual(writes.length,0);
messages.length=0; window.bridgeMode('7'); input('cancel\x03'); assert.deepStrictEqual(messages,[{input:'\x03'}]); messages.length=0; input('new\r'); assert.deepStrictEqual(messages,[{input:'new\r\n'}]);
writes.length=0; const payload="guest 'output' \\ \u2028 </script>"; window.bridgeOutput(payload); assert.strictEqual(writes[0],payload);
resized(); assert.strictEqual(fits,2);
console.log('PASS: terminal startup, dimensions, cooked editing, CRLF paste, escape filtering, disabled input, raw input, echo mode, literal output, resize. Mock transport only.');
