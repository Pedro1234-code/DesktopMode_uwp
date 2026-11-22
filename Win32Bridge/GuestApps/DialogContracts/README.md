# Dialog integration fixture

This standalone MSVC v143 x64 project exercises real property-sheet and USER32
APIs as a guest PE. It is not part of shell startup and does not modify the main
solution configuration. No executable has been built or staged by the agent.

Build `DialogContracts.vcxproj` in Release/x64 when desired, then launch its PE
through Win32Bridge. It briefly creates a modeless sheet and closes it after
synchronous checks. Exit code zero means success; otherwise the exit code is
the first failing line in DialogContracts.cpp. A hang or runtime fault is also
a failure. Check the bridge runtime log for the process exit result.

Coverage: real PSM returns (including null and -1), lazy pages, handle/index
queries, selection veto/skipping, preferred page focus, dynamic tab stops,
showing children without stealing focus, measured resize, Apply failure,
reentrant Apply rejection, modeless completion and one release per page.
The resize and reentry assertions are bridge-specific guarantees rather than
requirements to pass this entire fixture against desktop comctl32.

The fixture has not been compiled or executed. Passing the native Python
reference probes does not validate these bridge integration paths.
