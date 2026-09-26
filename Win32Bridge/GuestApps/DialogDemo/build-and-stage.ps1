param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'DialogDemo.vcxproj'
$stageDirectory = Join-Path $PSScriptRoot '..\Staged'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "Visual Studio build tools were not found at $vsDevCmd"
}

& cmd.exe /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && msbuild `"$project`" /m /p:Configuration=$Configuration /p:Platform=x64"
if ($LASTEXITCODE -ne 0) { throw 'DialogDemo build failed.' }

New-Item -ItemType Directory -Force -Path $stageDirectory | Out-Null
Copy-Item -Force (Join-Path $PSScriptRoot "x64\$Configuration\DialogDemo.exe") (Join-Path $stageDirectory 'DialogDemo.exe')
& (Join-Path $PSScriptRoot 'sync-embedded.ps1') -Configuration $Configuration
Write-Host "Staged $Configuration x64 guest PE at $stageDirectory\DialogDemo.exe"
