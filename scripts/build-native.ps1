[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [switch]$Test
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakePath = if ($null -ne $cmake) {
    $cmake.Source
} else {
    'C:\Program Files\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
    throw "CMake 3.28 or newer is required: $cmakePath"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Build Tools 2022 with the x64 C++ workload is required.'
}

$installation = & $vswhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if ([string]::IsNullOrWhiteSpace($installation)) {
    throw 'The MSVC x64 toolchain is unavailable.'
}

$preset = "windows-x64-$($Configuration.ToLowerInvariant())"
& $cmakePath --preset $preset
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

& $cmakePath --build --preset $preset
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
}

if ($Test) {
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    & $ctestPath --preset $preset --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed with exit code $LASTEXITCODE."
    }
}
