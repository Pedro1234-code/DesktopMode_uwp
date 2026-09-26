[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string] $Path
)

$resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
if (-not [System.IO.Path]::GetExtension($resolvedPath).Equals('.exe', [System.StringComparison]::OrdinalIgnoreCase) -and
    -not [System.IO.Path]::GetExtension($resolvedPath).Equals('.dll', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Expected a PE executable or DLL: $resolvedPath"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$dumpbin = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' |
    Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($dumpbin) -or -not (Test-Path -LiteralPath $dumpbin)) {
    throw 'Could not locate dumpbin.exe in the installed Visual Studio toolchain.'
}

Write-Host "Guest PE: $resolvedPath"
Write-Host "Inspector: $dumpbin"
Write-Host ''
& $dumpbin /headers /dependents /imports $resolvedPath
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed with exit code $LASTEXITCODE."
}
