# CoreShell 10X

CoreShell 10X é uma shell UWP inspirada no design do Windows 10X, adaptada para rodar no Windows e no Xbox. O projeto recria uma experiência de desktop leve dentro de um único aplicativo: menu Iniciar, barra de tarefas, janelas, Task View, aplicativos internos e Web Apps.

> O CoreShell não substitui o sistema operacional do Xbox. Ele executa como um aplicativo UWP e respeita as limitações da plataforma.

## Destaques

- Interface inspirada no Windows 10X, com suporte a tema claro e escuro.
- Cursor personalizado e ajustes de escala/DPI para uso no Xbox.
- Menu Iniciar com aplicativos do sistema, Web Apps e itens fixados.
- Barra de tarefas com indicadores de janela aberta, menus de contexto e persistência de itens fixados.
- Task View, alternância com `Alt` + `Tab` e gerenciamento de foco entre janelas.
- Web Apps instaláveis com WebView2, favicon, várias janelas, redimensionamento, minimizar, maximizar e fechar.
- Files: explorador de arquivos baseado nas permissões UWP e no seletor de pastas, incluindo mídia removível quando autorizada pelo usuário.
- Notepad: abertura e edição de arquivos `.txt`, salvar e salvar como.
- Settings: aplicativo interno com páginas de personalização, taskbar, informações do dispositivo e Windows Update visual.
- Action Center próprio com volume, estado de rede e atalhos para configurações do sistema.

## Estrutura

| Pasta/projeto | Descrição |
| --- | --- |
| `factoryos-10x-shell` | Aplicativo UWP principal e interface do CoreShell. |
| `factoryos-10x-shell.Library` | Modelos, serviços e view models compartilhados. |
| `Windows10x-js-main` | Referência HTML/CSS/JS da recriação original do Windows 10X. |
| `Win32Bridge` | Componentes auxiliares de integração, quando aplicáveis. |

## Compilação

1. Instale o Visual Studio 2022 com a carga de trabalho **Desenvolvimento para Plataforma Universal do Windows**.
2. Instale o Windows 10 SDK `10.0.19041` ou compatível.
3. Abra [`factoryos-10x-shell.sln`](factoryos-10x-shell.sln).
4. Selecione a arquitetura correta:
   - `x64` para teste no PC;
   - `ARM` para o Xbox, quando estiver usando o ambiente de desenvolvimento compatível.
5. Compile e inicie pelo Visual Studio.

Caso o Visual Studio informe que o certificado de assinatura não está disponível, gere ou associe um certificado de desenvolvimento ao pacote antes de implantar.

## Limitações conhecidas

- O acesso a arquivos fora do armazenamento do aplicativo depende de o usuário conceder acesso pelo seletor de arquivos/pastas do UWP.
- Aplicativos externos são abertos por URI/protocolo quando o pacote oferece suporte a isso.
- O botão de Windows Update no Settings é apenas visual por enquanto; atualizações reais continuam sendo responsabilidade do sistema.
- Alguns comportamentos podem variar entre PC, Xbox One e Xbox Series devido às APIs e restrições do UWP/Xbox.

## Créditos e licença

O projeto parte da recriação original de Windows 10X shell de [Pdawg-bytes](https://github.com/Pdawg-bytes/factoryos-10x-shell), expandida para o CoreShell com recursos voltados ao Xbox e desktop.

Distribuído sob a licença [MIT](LICENSE).
