# File descriptions, duplication and inherited stdio

GuestStorage now separates process handles (tokens and inherit/protect flags)
from the shared file description (IRandomAccessStream, cursor, access/share
metadata, description lock and final-close state). DuplicateFile and
DuplicateHandle with SAME_ACCESS add references to that description. Closing
one handle does not dispose the stream used by another process or duplicate.
The stream closes once, after the last handle, serialized against ongoing I/O.

Storage contexts using the same LocalFolder root share an open-description
registry, delete-pending state, namespace mutex and byte-range lock registry.
Create/open, delete and move checks consult all open descriptions rather than
only the current process table. Byte-range lock owners include their process
context so an inherited numeric token cannot release a parent's locks. This
does not make the preexisting byte-range I/O enforcement/blocking complete.

CreateFileW honors SECURITY_ATTRIBUTES.bInheritHandle; CreateFileA now has a
real conversion adapter through that same path. Get/SetHandleInformation and
current-process DuplicateHandle route file handles to storage. Only SAME_ACCESS
duplication is supported; reducing rights or duplicating into an already
running different process remains unsupported.

Before launching a child, CreateProcess validates supported startup handles and
copies inheritable file handles into the child's own storage table, preserving
the numeric values and shared descriptions. Preparation failures release that
child table normally. Startup stdio is applied after console allocation.
Process cleanup drops only its references; delete-on-close and pending deletes
wait for all open descriptions in the namespace. No reopen by pathname occurs.
Storage token allocation is global/monotonic within its reserved range, so
unrelated process handles do not accidentally alias byte-range owners.

Validation: Test-FileHandles.py passed 24 native Windows contracts including
shared cursor, source close, sharing denial, actual native child stdout writes,
child exit and final-reference deletion. These do not execute the bridge.
GuestApps/FileHandleContracts supplies an unbuilt integration PE which launches
itself as a child through the actual CreateProcess adapter and verifies the
same stream/cursor/lifetime path. No C++ build or guest execution was performed.
Source delimiters, project XML and diffs were checked; export alias check passed.

Remaining limitations include concurrent-open reservation races between the
share check and physical WinRT open, open-handle rename/path bookkeeping,
complete byte-range enforcement, ACL/security descriptors, and rights-reducing
or cross-process DuplicateHandle. CRT standard descriptors/FILE streams now
route to startup handles (CRT-STREAMS.md). Terminal presentation remains pending;
CMD and shell redirection have not passed runtime validation in the bridge.
