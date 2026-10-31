# Age of Empires memory investigation — log set 22

This is a source investigation and diagnostic change, not a verified leak fix.
No build or Xbox execution was performed. Shared engine source is under
`C:\firefox-source\engine\firefox`; rebuilding the engine and repackaging the
host are required. Both host versions use the engine instrumentation.

## What the existing capture establishes

- At 13:24:00.999 a Wasm compilation logs a 55,579,340-byte code section.
- At 13:24:02.190 memory is critical. The host records approximately 4,617 MiB
  charged against a 5,120 MiB application limit. Earlier usage was around 2,560 MiB.
- At 13:24:02.214 the memory policy refuses a fresh collection because its
  predicted 4,761 ms cost exceeds the configured 3,000 ms cap. It sends the
  ongoing-pressure notification instead.
- At 13:24:02.450 JS reports out of memory. A buffered compile then runs from
  13:24:04.324 to 13:24:05.638. Host usage reaches about 5,034 MiB.
- The first recorded graphics reset follows at 13:24:06.712; subsequent rendering
  falls back to software. This chronology does not establish a graphics leak.
- Fatal failures later include allocations of only 4 KiB. Their size is the last
  failed request, not the amount responsible for exhausting the quota.

The local xul PDB GUID matches the log's `707423a3-2038-693d-4c4c-44205044422e`,
age 1. Symbolized frames include `nsPipe::AdvanceReadSegment`,
`nsStringInputStream::ReadSegments`, `FetchDriver::OnDataAvailable`, and
`ProfilingStack::ensureCapacitySlow`. They identify failing allocations after
exhaustion, not the owner of all retained memory. Available physical RAM and free
virtual address space do not remove the application's commit/quota limit.

## Findings in the code

1. **Integrated host loses downward pressure notifications.**
   `GeckoHost.cpp::InitializeMemoryPressure` forwards increases with a host-side
   throttle, but its `AppMemoryUsageDecreased` handler only logs. In contrast,
   `GeckoW10mMemoryPressure.cpp::gecko_w10m_memory_pressure` expects every level
   change, including level zero, to update `sShellLevel` and end the pressure spell.
   The host can therefore leave a stale nonzero level in the engine. This is a
   concrete mismatch, but it does not explain the initial multi-GB growth by itself.

2. **The fresh-GC cost cap was actually reached.**
   `FreshBlocked` in the same engine file suppresses a fresh collection over
   `kFreshCostCapMs = 3000`. An ongoing notification does not replace that full JS
   collection. Retained unreachable objects are a hypothesis; current captures
   cannot establish how much could have been collected. GC policy is unchanged
   in this diagnostic pass so that measurements describe the existing failure.

3. **Two deferred-free paths can retain native buffers temporarily.**
   `nsSegmentedBuffer::Clear` queues large segment arrays on the background pool.
   Separately, `nsPipeEvents` accumulates popped segments and queues large batches.
   Under CPU/queue pressure they can outlive logical consumption. New independent
   counters cover both paths, including pipe task destruction on dispatch failure.
   Their freeing thresholds and dispatch behavior are unchanged.

4. **Buffered Wasm compilation owns an additional bytecode copy.**
   `WasmJS.cpp::GetBytecodeBuffer` copies its input; `CompileBufferTask` owns that
   buffer and roots its import object while queued. The existing large-task
   serialization limits simultaneous compilation, not the bytes retained by
   queued jobs. The original ArrayBuffer and compiled code can coexist with this
   copy. This is a possible peak/retention source, not a proven permanent leak.
   These copies may appear under unclassified heap rather than the Wasm bucket.

5. **The software compositor reuses its frame allocations.**
   `HeadlessCompositorWidget` replaces its `UniquePtr` when capacity grows and
   reuses it otherwise. Its destructor removes the widget from `sWindows`.
   The inspected paths do not allocate and retain a new full frame on each paint.
   This does not exclude hidden retained widgets or allocations in graphics APIs.

## New measurements and their limits

`GeckoW10mMemoryDiagnostics.cpp` emits fixed-field `memdiag:` lines to the existing
notes log. No report paths, URLs, descriptions, input, or document titles are
written. Detailed reporting requests anonymization and only the current process;
it does not request memory minimization or forced GC.

Light samples poll every two seconds, logging every ten seconds, on at least
128 MiB growth since the last logged sample, or on a pressure-level change:

| Field | Meaning |
| --- | --- |
| `commit_mb`, `resident_mb` | Process private committed bytes and working set |
| `app_total_mb`, `app_available_mb`, `app_limit_mb` | OS app commit charge, remaining charge, and their sum |
| `heap_mb`, `heap_overhead_pct` | Allocator live allocations and overhead fraction, including retained unused pages |
| `js_gc_mb`, `js_temp_peak_mb` | Main JS runtime GC heap and its reported temporary peak; excludes other worker runtimes |
| `ghost_windows` | Gecko's ghost-window count; a lead for retained documents, not proof of a leak |
| `streams_owned_mb` | Actual allocator sizes of segments still owned by segmented buffers |
| `streams_pending_free_mb` | Segments detached by Clear whose freeing callable has not finished |
| `pipe_pending_free_mb` | Consumed pipe segments awaiting destruction, including background batches |
| `process_error`, `app_error` | Win32 query errors; a failed query's zero fields mean unavailable |

Unsupported distinguished counters use `-1`. Stream counters exclude pointer
arrays and unrelated stream implementations; they are diagnostic subsets of
heap, never amounts to add to it. Separate atomic reads can have transient skew.
The `mb` suffix means MiB. App and process accounting are different measurements.

Detailed `categories` lines sum nonoverlapping `explicit/` report leaves into
Wasm, ArrayBuffer, network/streams, graphics/images/WebGL, JS, DOM, and other.
Names are classified in memory and discarded. `heap_unclassified_mb` estimates
allocator allocations outside reported heap leaves. It is not additional memory
outside `heap_mb`. Reports are asynchronous rather than an atomic snapshot;
`duration_ms` and commit before/after show drift during collection. Graphics
totals are not a complete GPU/driver allocation inventory.

Detailed reports normally wait 60 seconds and require a change since the last
request. Growth of at least 256 MiB can shorten the gap to 15 seconds. A report
taking over 250 ms backs off to 180 seconds. Reports are skipped without at least
512 MiB and 20% of app quota remaining; if the app query is unavailable, private
commit must be below 3 GiB. Lightweight samples continue near exhaustion. A
45-second report timeout does not cancel the manager's outstanding work; it only
allows diagnostics to recover and log a later request failure/retry.

## Reading the next reproduction

Compare the baseline before loading the game, the rising samples, and the state
after leaving its tab. Native pending-free bytes rising then falling implicate
deferred reclamation. Heap remaining high with low pending-free counters requires
checking the category reports. High overhead with falling live heap suggests
allocator retention/fragmentation. Persistent JS/Wasm/DOM allocations warrant
examining object roots and queued tasks; growth alone does not distinguish a
reachable object from an unreachable one awaiting collection. App charge growing
without corresponding process private/heap growth warrants graphics/other-process
accounting investigation.

These checks narrow the responsible allocation family. A dump/profiler or
additional targeted ownership counters may still be needed to name the leaking
object. No conclusion here treats the unexplained growth as the game's legitimate
working set.

Source-contract and lifetime-model checks (no compilation):

```text
node tests/memory-diagnostics.test.cjs C:\firefox-source\engine\firefox
```

Reporting semantics follow Mozilla's
[memory reporter documentation](https://firefox-source-docs.mozilla.org/performance/memory/memory_reporting.html).
