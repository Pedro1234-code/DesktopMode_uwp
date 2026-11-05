## Enable Loopback
In order to be able to use Xbox Device Portal directly from Strawfox, and use the Appx Installer feature, you must enable loopback every reboot. Here's how to do it:

- Start an SSH connection using whatever client on your phone or PC (needs to be on the same network as the Xbox)
- User: DevToolsUser
- Password: the Visual Studio PIN showed on Dev Home

Once connected to the shell, run: CheckNetIsolation.exe LoopbackExempt -a -n="Microsoft.MicrosoftEdge.CoreShell_8wekyb3d8bbwe"

You must do it every time the console boots.
