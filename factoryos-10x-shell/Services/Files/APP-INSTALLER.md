# Embedded App Installer

Files routes `.appx`, `.appxbundle`, `.msix`, and `.msixbundle` to an independent
App Installer window. The window participates in activation, Task View, Alt+Tab,
minimization, and closing. Closing during deployment minimizes it: a submitted
Device Portal deployment cannot safely be cancelled by merely closing its UI.

Metadata is read on a worker using .NET ZipArchive and bounded XML parsing with
DTDs disabled. Only the manifest and selected logo are loaded into memory.
Bundles select the current Shell architecture or neutral application package;
only that nested package is streamed to a unique temporary file, then deleted.
This avoids extracting all architectures and all payload files. Very large
bundles still need temporary space for that one embedded package.

Direct display strings, identity, version, publisher, description, capabilities,
and exact/scale-qualified logos are supported. Unresolved `ms-resource:` strings
fall back to identity/publisher with an explanatory UI notice. External PRI and
resource-only bundle packs are not resolved. Metadata is package-supplied and
is not presented as a verified publisher/trust verdict.

Deployment uses Windows.Web.Http streaming multipart at the fixed local address
https://localhost:11443. Optional dependency packages can be selected. Credentials
are used in memory only and the password field is cleared after each attempt.
The local self-signed-certificate option relaxes only untrusted-chain/name checks
on this client; redirects are disabled. No global TLS policy is changed.

Only one deployment is permitted in-process, and preflight checks whether the
portal already has a running deployment. The portal state endpoint is global:
another external Device Portal client should not deploy concurrently. Polling
requires an explicit successful result; upload acceptance is not completion.
Timeouts report that the remote operation may continue. No destructive uninstall
of an existing package is performed before deploying an update.

After Portal reports deployment success, resolve the installed package by its
identity prefix in PackageFullName (the Portal's Name can be a friendly display
name). Verify registration with FindPackageForUser for the calling Shell user.
If absent, use RegisterPackageByFullNameAsync with DeploymentOptions.None, then
verify again before refreshing Start or launching. No users are hardcoded and
no all-user provisioning or uninstall is performed. This uses the existing
installer connection only; Start discovery does not depend on loopback.
Failure to verify/register is reported as a post-install warning, not a failed
upload or a request to repeat installation. AppInstaller-runtime.log records
package identity, registration presence and error HRESULT, without credentials
or user SIDs. Xbox support/permission for registration still needs device validation.

The Xbox's own tools/XboxAppLauncher/XboxAppLauncher.js calls WebbRest.installApp;
js/rest.js posts multipart to /api/app/packagemanager/package?package=filename,
the same endpoint used here, with no user parameter. Observed on 2026-11-05:
DOSBox Pure UWP registered only for DefaultAccount; Shell and XBSX2 registered
for both DefaultAccount and UserMgr0. This establishes a registration difference,
but does not prove which client originally caused it.

References consulted:
- https://github.com/Empyreal96/WP_Device_Portal
- https://github.com/microsoft/WindowsDevicePortalWrapper
- https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core
- https://learn.microsoft.com/en-us/uwp/api/windows.management.deployment.packagemanager.registerpackagebyfullnameasync
- https://learn.microsoft.com/en-us/windows/msix/app-installer/app-installer-ui-dialog
- https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.resources.core.resourcemanager.loadprifiles

Validation: XML structure, XAML event handlers, project inclusion, scoped diff
review. No build or live Device Portal deployment performed.
