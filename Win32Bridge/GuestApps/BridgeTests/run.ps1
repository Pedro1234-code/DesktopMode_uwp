$ErrorActionPreference = 'Stop'

$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
$project = Join-Path $PSScriptRoot 'BridgeTests.vcxproj'
$guest = Join-Path $PSScriptRoot '..\Staged\DialogDemo.exe'

if (-not (Test-Path -LiteralPath $guest)) {
    throw 'Stage DialogDemo.exe before running this test.'
}

& cmd.exe /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && msbuild `"$project`" /m /p:Configuration=Release /p:Platform=x64"
if ($LASTEXITCODE -ne 0) { throw 'BridgeTests build failed.' }

& (Join-Path $PSScriptRoot 'x64\Release\BridgeTests.exe') $guest
if ($LASTEXITCODE -ne 0) { throw 'BridgeTests failed.' }
