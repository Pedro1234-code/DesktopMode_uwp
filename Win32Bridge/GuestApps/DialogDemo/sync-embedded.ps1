param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$guest = Join-Path $PSScriptRoot "x64\$Configuration\DialogDemo.exe"
$header = Join-Path $PSScriptRoot '..\DialogDemoEmbedded.h'

if (-not (Test-Path -LiteralPath $guest)) {
    throw "Guest PE was not found at $guest. Build DialogDemo first."
}

$base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($guest))
$chunks = for ($offset = 0; $offset -lt $base64.Length; $offset += 120) {
    $length = [Math]::Min(120, $base64.Length - $offset)
    '            L"' + $base64.Substring($offset, $length) + '"'
}

$lines = @(
    '#pragma once',
    '',
    'namespace Win32Bridge',
    '{',
    'namespace GuestAssets',
    '{',
    '    inline Platform::String^ DialogDemoBase64()',
    '    {',
    '        return ref new Platform::String('
) + $chunks + @(
    '            );',
    '    }',
    '}',
    '}'
)

[IO.File]::WriteAllLines(
    $header,
    [string[]]$lines,
    [Text.UTF8Encoding]::new($false))

Write-Host "Embedded $Configuration x64 guest into $header"
