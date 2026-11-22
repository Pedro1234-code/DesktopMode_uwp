# File handle integration PE

Standalone MSVC v143 Release/x64 fixture, not added to the main solution and
not built/staged by the agent. Run its PE through Win32Bridge; exit 0 indicates
success, otherwise the exit code is the first failed source line. If a child
fails, inspect its separate exit code in the runtime diagnostics.

The parent creates a uniquely named temporary file, duplicates an inheritable
handle, closes the original and starts the same PE as a child with that file
as stdout/stderr. The child verifies that a conflicting open is denied and
writes through stdout. After the child exits, the parent checks the shared
cursor and writes/reads again, proving child cleanup did not close its handle.
It also checks per-handle close protection and final-reference delete-on-close.
An inherited event gates the child while the parent closes the original process
handle and both thread handles, then reopens them by ID. The process duplicate
and reopened thread still signal, and the reopened handles report the exit code.
The same PE also checks `ExitProcess` called from a worker while the main thread
is in `SleepEx`, and `ExitThread` called from the main thread while a worker
is still running. Its parent observes the primary thread exit before releasing
the worker and checking the final process exit status.
Multi-object waits cover all-or-nothing consumption, lowest-index selection,
invalid handles, aliases, mutex abandonment, APC interruption and the message
queue requirement of MWMO_WAITALL.

The mapping cases cover independent view addresses, shared visibility,
copy-on-write isolation, interior-address flushes and named handle access.
The file-backed test closes the original handles, renames the backing file,
reuses its old path, and checks both files after the flush.

`ReplaceFileW` cases cover missing targets, share violations, replacement of
closed files, and both backup modes with the original open. The open original
still reads its old bytes, while new opens see the replacement. The optional
backup keeps the original's byte-range locks until their owner releases them.

File information tests call the Win32 and NT entry points. They check that EOF
updates preserve the cursor, renames work through the original handle, and
delete disposition can be queried and cleared through a second eligible
handle. Directory tests reject nonempty directories and prevent creation
inside a directory marked for removal.

The loader cases use counted NT strings and look up an export in the main
image by name and ordinal. A temporary copy of the fixture exercises an
explicit DLL search list, mapping without initialization, shared Win32/NT
references and unloading after the last release.

The fixture has not been compiled or executed. Its native Python reference
probe is separate and does not validate the bridge implementation. On failure
while a child is still running, its temporary file may remain until cleanup.
