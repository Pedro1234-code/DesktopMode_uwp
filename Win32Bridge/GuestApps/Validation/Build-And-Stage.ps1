$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.Component.MSBuild -find MSBuild\Current\Bin\MSBuild.exe |
    Select-Object -First 1
if (-not $msbuild) { throw 'Visual Studio 2022 MSBuild was not found.' }

$project = Join-Path $PSScriptRoot 'Validation.vcxproj'
$apps = 'CrtValidation', 'SystemValidation', 'ConsoleValidation', 'Win32Gallery', 'FileHandleContracts', 'GraphicsContracts'
foreach ($app in $apps) {
    & $msbuild $project /nologo /m /v:minimal /t:Build /p:Configuration=Release /p:Platform=x64 "/p:ValidationApp=$app"
    if ($LASTEXITCODE -ne 0) { throw "$app failed to build." }
}
$stage = Join-Path $PSScriptRoot '..\Staged\TestApps'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($app in $apps) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "x64\Release\$app\$app.exe") -Destination $stage -Force
}
