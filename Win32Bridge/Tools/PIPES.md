# Anonymous pipe and startup stdio implementation

GuestPipeContext owns a separate handle table per guest process. Each handle
entry owns one endpoint, with separate reader/writer counts in a shared byte
queue. Duplicate/inherited handles get independent endpoint objects. Closing a
handle marks that endpoint closed immediately, so an outstanding I/O reference
does not keep a writer alive and suppress EOF. Process teardown closes even
protected handles. Inheritance snapshots are adopted by the child kernel, not
retained as a second live table after startup.

Implemented routes: CreatePipe, PeekNamedPipe for anonymous pipes, GetFileType,
ReadFile/WriteFile, CloseHandle, Get/SetHandleInformation and DuplicateHandle
for pipe/console/file handles in the current process with DUPLICATE_SAME_ACCESS.
CreatePipe supports optional inheritance; non-null security descriptors fail
explicitly because guest ACL evaluation is not implemented. Reads drain
buffered bytes before EOF. Writes block while capacity is exhausted; both
paths pump guest messages between retries without holding a pipe lock.

Measured native semantics are preserved: wrong-direction access is access
denied; final-writer EOF is ERROR_BROKEN_PIPE; writing without readers is
ERROR_NO_DATA (including zero-byte writes); protected CloseHandle reports
ERROR_INVALID_HANDLE. Internal waiting uses ERROR_IO_PENDING only between the
transport and synchronous shim, never as a successful async I/O claim.
Peek's remaining-message count is not meaningful for byte pipes; the adapter
returns zero rather than reproducing Windows' observed underflow value.

CreateProcess accepts inherited STARTF_USESTDHANDLES for supported pipe,
console and file endpoints. Inheritable pipe handles keep their numeric values in the
child's separate table. Non-inheritable endpoints are omitted. Explicit stdio
is restored after console allocation so startup cannot overwrite a pipe with
a console handle. NEW_CONSOLE/DETACHED console choices remain separate from
pipe inheritance. Disk/file inheritance now shares the original open description
and position (see FILE-HANDLES.md); no path reopen is used.

Still pending: named pipes, overlapped pipe I/O, waitable pipe handles, full
cancellation/APC/IOCP semantics, cross-process DuplicateHandle targeting an
already running process, access-mask reduction in DuplicateHandle, and handle
information/duplication for other object families. CRT descriptors/FILE streams
now route to anonymous pipes (CRT-STREAMS.md). The terminal UI and current CMD source-import audit are documented in
TERMINAL.md and CMD-COMPATIBILITY.md. Shutdown now cancels synchronous pipe
retry loops cooperatively; complete I/O cancellation remains pending.

Validation: Test-Pipes.py passed 43 native Windows reference contracts without
showing UI. Those checks do NOT execute bridge C++. PipeFixtures.h is wired
into BridgeTests for queue capacity, byte order, flags, inheritance, ownership
transfer and EOF, but has NOT been built/run by the agent. Child guest process
startup and shell pipelines have not been executed. The user owns builds.
