[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-StrictDescendant {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $normalizedRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($Root))
    $normalizedCandidate = [IO.Path]::GetFullPath($Candidate)
    if (-not $normalizedCandidate.StartsWith(
        $normalizedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw "Release work path escaped the validated artifacts root: $normalizedCandidate"
    }
}

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage Exit code: $LASTEXITCODE"
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifactsRoot = Join-Path $repositoryRoot 'artifacts'
$outputRoot = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory($artifactsRoot) | Out-Null
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null

$releaseWorkLeaf = "release-work-$Version"
$releaseWork = Join-Path $artifactsRoot $releaseWorkLeaf
$hostPublish = Join-Path $releaseWork 'rmm-bridge-host'
$launcherPublish = Join-Path $releaseWork 'launcher'
Assert-StrictDescendant -Candidate $releaseWork -Root $artifactsRoot
if ([IO.Path]::GetFileName($releaseWork) -cne $releaseWorkLeaf) {
    throw "The release work directory has an unexpected name: $releaseWork"
}

$nativeBuildScript = Join-Path $repositoryRoot 'scripts/build-native.ps1'
Invoke-CheckedCommand `
    -FilePath 'pwsh' `
    -Arguments @(
        '-NoProfile',
        '-File',
        $nativeBuildScript,
        '-Configuration',
        'Release',
        '-Test') `
    -FailureMessage 'Native Release build or tests failed.'

if (Test-Path -LiteralPath $releaseWork) {
    Remove-Item -LiteralPath $releaseWork -Recurse -Force
}
[IO.Directory]::CreateDirectory($hostPublish) | Out-Null
[IO.Directory]::CreateDirectory($launcherPublish) | Out-Null

$bridgeDll = Join-Path $repositoryRoot (
    'native/out/build/windows-x64-release/native/runtime/Release/' +
    'DSRRandomizer.RmmBridge.dll')
if (-not (Test-Path -LiteralPath $bridgeDll -PathType Leaf)) {
    throw "The native Release bridge is missing after the verified build: $bridgeDll"
}

$hostProject = Join-Path $repositoryRoot (
    'src/DSRRandomizer.RmmBridgeHost/DSRRandomizer.RmmBridgeHost.csproj')
Invoke-CheckedCommand `
    -FilePath 'dotnet' `
    -Arguments @(
        'publish',
        $hostProject,
        '-c',
        'Release',
        '-r',
        'win-x64',
        '-nr:false',
        '--self-contained',
        'true',
        '-p:PublishSingleFile=true',
        '-p:IncludeNativeLibrariesForSelfExtract=true',
        '-o',
        $hostPublish) `
    -FailureMessage 'Self-contained bridge-host publish failed.'

$hostExecutable = Join-Path $hostPublish 'DSRRandomizer.RmmBridgeHost.exe'
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
    throw "The self-contained bridge host is missing: $hostExecutable"
}

$launcherProject = Join-Path $repositoryRoot (
    'src/DSRRandomizer.Launcher/DSRRandomizer.Launcher.csproj')
Invoke-CheckedCommand `
    -FilePath 'dotnet' `
    -Arguments @(
        'publish',
        $launcherProject,
        '-c',
        'Release',
        '-r',
        'win-x64',
        '-nr:false',
        '--self-contained',
        'true',
        '-p:PublishSingleFile=true',
        '-p:IncludeNativeLibrariesForSelfExtract=true',
        "-p:PinnedBridgePath=$bridgeDll",
        "-p:PinnedBridgeHostPath=$hostExecutable",
        '-o',
        $launcherPublish) `
    -FailureMessage 'Pinned self-contained launcher publish failed.'

$publishedBridgeRoot = Join-Path $launcherPublish 'components/rmm-bridge'
$publishedBridge = Join-Path $publishedBridgeRoot 'DSRRandomizer.RmmBridge.dll'
$publishedHost = Join-Path $publishedBridgeRoot 'DSRRandomizer.RmmBridgeHost.exe'
foreach ($path in @($publishedBridge, $publishedHost)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "The official launcher publish omitted a pinned bridge artifact: $path"
    }
}

$bridgeHash = (Get-FileHash -LiteralPath $publishedBridge -Algorithm SHA256).Hash.ToLowerInvariant()
$hostHash = (Get-FileHash -LiteralPath $publishedHost -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [ordered]@{
    schemaVersion = 1
    configuration = 'Release'
    bridgeSha256 = $bridgeHash
    hostSha256 = $hostHash
} | ConvertTo-Json -Compress
[IO.File]::WriteAllText(
    (Join-Path $publishedBridgeRoot 'deployment-manifest.json'),
    $manifest,
    [Text.UTF8Encoding]::new($false))

$dependencyManifest = Join-Path $repositoryRoot (
    'src/DSRRandomizer.Launcher/bin/Release/net8.0-windows/win-x64/' +
    'DSRForMod.Launcher.deps.json')
if (-not (Test-Path -LiteralPath $dependencyManifest -PathType Leaf)) {
    throw "The exact launcher publish dependency manifest is missing: $dependencyManifest"
}

$packageScript = Join-Path $repositoryRoot 'packaging/package.ps1'
Invoke-CheckedCommand `
    -FilePath 'pwsh' `
    -Arguments @(
        '-NoProfile',
        '-File',
        $packageScript,
        '-Version',
        $Version,
        '-PublishPath',
        $launcherPublish,
        '-DependencyManifestPath',
        $dependencyManifest,
        '-OutputPath',
        $outputRoot) `
    -FailureMessage 'Verified release packaging failed.'
