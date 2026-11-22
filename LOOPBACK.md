## Enable Loopback (en)
In order to be able to use Xbox Device Portal directly from Strawfox, and use the Appx Installer feature, you must enable loopback every reboot. Here's how to do it:

- Start an SSH connection using whatever client on your phone or PC (needs to be on the same network as the Xbox)
- User: DevToolsUser
- Password: the Visual Studio PIN showed on Dev Home

Once connected to the shell, run: `CheckNetIsolation.exe LoopbackExempt -a -n="Microsoft.MicrosoftEdge.CoreShell_8wekyb3d8bbwe"`

You must do it every time the console reboots.

## Habilitar loopback 🇧🇷

Para usar o App Installer direto do Shell, você tem que habilitar o acesso do app ao IP local do Xbox (loopback).

1. Baixe algum cliente SSH qualquer na Play Store ou App Store, com um celular conectado na mesma rede do Xbox. Para Android, recomendo o https://play.google.com/store/apps/details?id=com.johnan.sshclient por ser bem fácil.
2. No celular, insira o IP do console, exibido na Dev Home. O nome do usuário é DevToolsUser.
3. Ele vai pedir uma senha pra conectar o SSH. A senha é o PIN do Visual Studio obtido na Dev Home, no botão "Show Visual Studio PIN".
4. No CMD, cole `CheckNetIsolation.exe LoopbackExempt -a -n="Microsoft.MicrosoftEdge.CoreShell_8wekyb3d8bbwe"`, vai aparecer "OK."
5. Só baixar um .appx/msix no Xbox e dar dois cliques.

Só precisa fazer isso denovo caso o console seja reiniciado.
