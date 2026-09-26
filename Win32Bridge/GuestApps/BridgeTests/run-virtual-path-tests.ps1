$ErrorActionPreference = 'Stop'

$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$project = Join-Path $PSScriptRoot 'VirtualPathContractTests.vcxproj'

& cmd.exe /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && msbuild `"$project`" /m /p:Configuration=Release /p:Platform=x64"
if ($LASTEXITCODE -ne 0) { throw 'VirtualPathContractTests build failed.' }

& (Join-Path $PSScriptRoot 'x64\Release\VirtualPathContractTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'VirtualPathContractTests failed.' }
