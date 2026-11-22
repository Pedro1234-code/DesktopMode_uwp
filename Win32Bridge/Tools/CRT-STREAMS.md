# CRT streams and guest stdio

Each GuestModuleLoader initializes CRT descriptors 0/1/2 from its explicit
GuestKernelContext after console allocation and startup redirection. No host
stdin/stdout or TLS context is used to discover startup handles. Null/invalid
handles leave the corresponding descriptor unavailable. Repeated initialization
does not replace or close an existing descriptor.

The file backend identifies disk, console and pipe handles independently of
GetFileSize. _open_osfhandle accepts supported non-seekable endpoints. _isatty
uses the endpoint type. _open applies _O_NOINHERIT to the guest file handle.
_dup/_dup2 duplicate process handles, share the underlying open description,
clear NOINHERIT and retain independent lowio text/lookahead state. _dup2 to
0/1/2 publishes the duplicate via SetStdHandle; _close clears that standard slot.
Initial stdout/stderr with identical raw handle values share their ownership
wrapper to avoid double close. Duplicate descriptors do not replay private
lookahead bytes already consumed from the pipe by the source descriptor.

Existing printf/fwrite and getchar/fread implementations reach the same CRT
file backend. Disk operations use GuestStorage. Console/pipe operations use
the synchronous Win32 bridge shims, including their guest message-pump retry.
CRT maps the final-writer BROKEN_PIPE read to successful EOF, while failed
writes to a pipe without readers report EPIPE and FILE error state.

Text mode translates CRLF and expands output LF. Non-seekable CR and UTF-8
lookahead is retained per descriptor; disk lookahead continues to use seek.
Short physical UTF-16 reads assemble a code unit without discarding bytes.
For _O_U8TEXT stdio, lowio can return both UTF-16 units of one UTF-8 scalar.
The FILE retains the second unit for the next wide read without seeking the
underlying disk or pipe. ANSI wide stdio follows guest LC_CTYPE, independently
of _setmbcp. Wide formatted output honors its explicit locale for ANSI bytes.
Valid UTF-8 sequences of three and four bytes round-trip in ANSI streams when
LC_CTYPE is UTF-8. This extends the observed native ANSI fgetwc behavior.
Truncated DBCS lead bytes are made available to subsequent byte reads.

ANSI text writes to the virtual console retain incomplete UTF-8 and DBCS
prefixes per descriptor, including across stdio flushes and lowio batches.
A failed console batch leaves the previous prefix available for retry.
Changing text mode clears it. Changing the source page finishes a pending
prefix using the previous source page. Console writes decode the guest
LC_CTYPE page and convert to the active console output page with Windows
substitution. The C locale uses the console output page for narrow writes.
Buffered stdio remembers the source page, including for explicit wide-output
locales, until it sends those bytes. Disk, pipes and binary writes keep their
existing byte semantics.
Unicode console modes use ReadConsoleW/WriteConsoleW, while redirected Unicode
streams use their UTF-8/UTF-16 byte encoding. No implicit BOM is written merely
by adopting a redirected handle. Device Ctrl-Z handling follows the distinction
in ReactOS's sdk/lib/ucrt/lowio/read.cpp. dup2.cpp, ioinit.cpp and write.cpp were
also consulted as behavioral references; their source was not copied here.

A buffered write can fill the buffer exactly without flushing it. Direct
short writes report completed elements and set the FILE error flag. The bridge
keeps undelivered buffered data available for an explicit flush retry. If a
line-triggered flush fails, the call reports its delivered prefix and removes
its unreported suffix from the buffer so the application can retry it.

Text-mode lowio keeps physical progress within one interrupted logical unit.
When the next write on that descriptor starts with the same logical bytes,
it sends the remaining physical bytes of the unit, then continues with fresh
input. This works for a split CRLF, UTF-16 code unit or surrogate pair encoded
as UTF-8. A different prefix returns EINVAL while retaining the interrupted
unit. Reading, repositioning, resizing or changing the effective text mode
abandons that retry state. It is private to the descriptor and is not copied
by _dup. This recovery applies to retries on the same descriptor, without
interleaving writes through another handle to the same destination.

## Verification and limits

Test-CrtStreams.py passed 65 contracts against native Windows/UCRT in an
isolated Python subprocess: pipe adoption, CR lookahead, duplicate lifetime,
Ctrl-Z, stdout/SetStdHandle synchronization, physical CRLF bytes, FILE read/write
and EOF versus error. This probe never loads or executes bridge C++.
CrtStreamFixtures.h is wired into BridgeTests and exercises the actual CRT
classes with GuestConsoleContext/GuestPipeContext, including Unicode console,
short UTF-8/UTF-16 reads, lookahead ownership and broken-writer errors. These
fixtures have NOT been compiled or executed by the agent. User owns builds.
The fault-injection cases in CrtFileFixtures.h and CrtStdioFixtures.h cover
repeated short writes, mismatched retries, and the interaction with line
buffering. They check Bridge recovery behavior that the native reference
suite does not provide.

CreateProcessA/W now carries the STARTUPINFO reserved CRT table into the child.
The Bridge preserves descriptor indexes and translation flags for inheritable
guest handles and republishes the filtered table from GetStartupInfoA/W for
statically linked CRTs. CrtFileFixtures.h covers sparse tables, malformed counts,
append and text mode, and aliases of an inherited handle.
A native Windows/UCRT probe confirmed inheritance of descriptor 37, aliases
with stdout, per-descriptor text/binary flags above fd 2, and writes in the child.

The narrow and wide `_spawn` variants now generate the same descriptor packet.
They accept explicit environments, search the guest PATH for the `p` variants,
and support synchronous, asynchronous, and detached PE launches. `_cwait`
reports the full exit code and consumes the guest process handle. Process
replacement (`_P_OVERLAY`, `_exec`) and execution of batch scripts are pending.

Still pending: exact native buffering selection and full locale/code-page
coverage. Seek/size/commit on non-disk endpoints remain unsupported rather than
emulating the native pipe's synthetic seek/size results. Recursive descriptor
locks allow same-thread pump reentry; this does not prove arbitrary cross-thread
callback or close/read race equivalence. The terminal frontend is documented in TERMINAL.md; these stream checks do
not prove that cmd.exe is interactive. End-to-end child CRT redirection still needs a
user-run build and guest execution.

Locale-sensitive formatting and scanning use the guest's shared or private
CRT locale by default. Buffer printf/scanf and stream scanf accept explicit
native locale handles. See CRT-LOCALE.md for handle lifetime and thread visibility.

For stream scanf, string input skips locale whitespace before the field, then
ends the field only at the ASCII separators defined by UCRT. Wide formats
accept Unicode decimal field widths. Width overflow saturates at UINT64_MAX
and reports ERANGE. The secure scanner reports malformed formats through errno
while retaining prior assignments. An incomplete DBCS literal in the format
is detected before the input is read. Secure text conversion bounds the output
by the guest buffer size even on native UTF-8 configurations where the host CRT
can write beyond the advertised capacity. Test-CrtScanning.py covers native
reference behavior for cursor position, EOF, format errors and Unicode fields.
