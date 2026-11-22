# Guest console transport

The console belongs to a guest kernel/process, never to the host console.
PE metadata now records OptionalHeader.Subsystem. CUI guests automatically get
input/output/error handles, unless detached. GUI guests can call AllocConsole.
CloseHandle, GetFileType, Get/SetStdHandle, Get/SetConsoleMode, console A/W I/O,
and console code-page APIs route to this process-owned table. ReadFile and
WriteFile recognize console endpoints before consulting guest storage.

Child processes normally inherit the console session, with separate handle
tables and shared input/output/modes. Detach and closing a child's handle do
not invalidate a parent's handle. CREATE_NEW_CONSOLE and DETACHED_PROCESS
select independent/no-console state; CREATE_NO_WINDOW uses a new headless
session. No native desktop console or host handles are exposed.

GuestRuntime::FeedConsoleInput supplies cooked UTF-16 text (including newline
for line-mode reads) or explicit EOF. DrainConsoleOutput consumes the captured
text. The UWP xterm frontend consumes these hooks through a separately published
console transport snapshot, avoiding a race with initial Run setup. Console writes are also recorded as CONSOLE OUTPUT in
the runtime diagnostics so noninteractive output has an observable path now.
ReadConsole pumps messages on its guest thread while waiting for input; no
console mutex is held during guest callbacks. Process cleanup invalidates its
console endpoints so waiting reads fail instead of staying blocked forever.
Output remains available for draining after process exit.

CRT standard descriptors and FILE streams now route to these endpoints and
redirected pipes/files; see CRT-STREAMS.md for coverage and remaining gaps.

## Not implemented by this transport layer

- Complete INPUT_RECORD event APIs, resize records, line navigation/history,
  all console-device opens, AttachConsole and full VT interpretation. The UWP
  terminal and core cell/cursor/color/fill/scroll operations are implemented.
- ReadConsole control/wakeup structures (explicitly ERROR_NOT_SUPPORTED).
- Named pipes and cross-process DuplicateHandle to an already running process.
  Anonymous pipes and inherited console/pipe/file stdio are implemented (see
  PIPES.md and FILE-HANDLES.md). File descriptions share their stream and cursor
  across inherited handles, with separate process handle flags. SetStdHandle
  stores values without validating them, matching Win32.
- ANSI writes split in the middle of a multibyte sequence across calls.
  ANSI reads retain unreturned encoded bytes when caller buffers are small.
- Complete console mode semantics. Line/stream reads and frontend echo are
  supported; processed Ctrl+C is delivered cooperatively during console reads,
  with reverse-order handler invocation and a guest-only default exit policy.
  This does not implement asynchronous process-group signaling.

This is not a claim that Microsoft's cmd.exe already runs or is interactive.
Its exact imports and runtime behavior still need version-specific inspection.

## Verification

ConsoleFixtures.h is integrated into GuestApps/BridgeTests and directly tests
this context: std-handle validation, allocation, direction errors, cooked-line
reads, partial reads, UTF-8 byte retention, EOF, session sharing, independent
close/detach, disjoint sessions, and external redirect detection. It has NOT
been compiled or executed by the agent; the user owns builds. Source delimiter,
project XML and diff checks passed. Export alias check passed for 3729 aliases
from 20 ReactOS specs. These static checks do not establish runtime correctness.

ConsoleBufferFixtures.h additionally covers cell updates, cursor/attributes,
fill/scroll, control events and timers. It remains unbuilt/unrun. Native hidden-
console probes passed 19 ABI/buffer reference checks; frontend mock tests passed.
See CMD-COMPATIBILITY.md for the current source import audit and its limits.
