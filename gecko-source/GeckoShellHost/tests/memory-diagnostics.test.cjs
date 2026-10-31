// Source contracts and lifetime models only; this does not compile or run Gecko.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = process.argv[2];
assert(root, 'Pass the Firefox engine source directory.');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const diagnostics = read('widget/headless/GeckoW10mMemoryDiagnostics.cpp');
const segmented = read('xpcom/io/nsSegmentedBuffer.cpp');
const pipe = read('xpcom/io/nsPipe3.cpp');

assert.match(diagnostics, /#include <windows\.h>\s+#include <psapi\.h>/);
assert.doesNotMatch(diagnostics, /mozilla::(?:SprintfLiteral|RefPtr)\b/);
assert.match(diagnostics, /GetReportsForThisProcessExtended\(\s*report, nullptr, true, nullptr, report, nullptr\)/);
assert.doesNotMatch(diagnostics, /->(?:MinimizeMemoryUsage|GetReportsExtended)\(/);
assert.match(diagnostics, /!process\.IsEmpty\(\)/);
assert.match(diagnostics, /GetHeapOverheadFraction\(&overhead\)/);
assert.match(diagnostics, /process\.available >= 512 \* MiB/);
assert.match(diagnostics, /process\.available >= process\.limit \/ 5/);
assert.match(diagnostics, /sCheckpointPending\.exchange\(true\)/);
assert.match(diagnostics, /sActiveReport == mId/);
assert.match(diagnostics, /duration > 250 \? 180 : 60/);
assert.match(segmented, /sSegmentPendingFreeBytes -= pendingBytes/);
assert.match(pipe, /using PipeFreePtr = UniquePtr<char, GeckoW10mPipeSegmentDeleter>/);
assert.match(pipe, /free\(segment\);\s*sPipePendingFreeBytes -= bytes/);
assert.match(read('widget/headless/moz.build'), /"GeckoW10mMemoryDiagnostics\.cpp"/);
assert.match(read('widget/headless/GeckoW10mMemoryPressure.cpp'), /GeckoW10mMemoryDiagnosticsStart\(\);/);
for (const note of diagnostics.matchAll(/GeckoW10mNote\(([^;]+)\);/g)) {
  assert(!/\b(path|process|description)\b/.test(note[1]), 'No report path/content may be logged.');
}

// Ownership transfers: append, pop, shrink, Clear, and delayed task disposal.
class Counters {
  owned = 0; clearPending = 0; pipePending = 0;
  append(n) { this.owned += n; }
  pop(n) { this.owned -= n; }
  shrink(oldBytes, newBytes) { this.owned += newBytes - oldBytes; }
  clear(n) {
    this.owned -= n;
    this.clearPending += n;
    let live = true;
    return () => {
      assert(live, 'The freeing callable must run exactly once.');
      live = false;
      this.clearPending -= n;
    };
  }
  pipe(n) {
    this.pipePending += n;
    let live = true;
    return () => {
      assert(live, 'The owning pointer must be destroyed exactly once.');
      live = false;
      this.pipePending -= n;
    };
  }
}
for (const mode of ['synchronous', 'background', 'dispatch-failed']) {
  const c = new Counters();
  c.append(8192); c.append(8192);
  c.shrink(8192, 4096);
  c.pop(8192);
  const dropPipe = c.pipe(8192);
  const finishClear = c.clear(4096);
  assert.deepEqual([c.owned, c.clearPending, c.pipePending], [0, 4096, 8192]);
  // Pipe's unique pointer releases on execution OR destruction of a rejected task.
  dropPipe();
  // Clear retains its existing synchronous fallback when dispatch fails.
  finishClear();
  assert.deepEqual([c.owned, c.clearPending, c.pipePending], [0, 0, 0], mode);
  c.clear(0)();
  assert.equal(c.clearPending, 0, 'Repeated empty Clear must not underflow.');
}

const MiB = 1024 * 1024;
const headroom = (commit, available, limit) => limit
  ? available >= 512 * MiB && available >= limit / 5
  : commit < 3 * 1024 * MiB;
assert(headroom(2560 * MiB, 2560 * MiB, 5120 * MiB));
assert(!headroom(4617 * MiB, 503 * MiB, 5120 * MiB));
assert(!headroom(4200 * MiB, 920 * MiB, 5120 * MiB));
assert(!headroom(4000 * MiB, 0, 0));
assert(headroom(2000 * MiB, 0, 0));
console.log('Memory diagnostic source contracts and lifetime models passed (no native runtime validation).');
