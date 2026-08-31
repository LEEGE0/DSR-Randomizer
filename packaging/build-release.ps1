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
Import-Module (Join-Path $PSScriptRoot 'SafeReleaseDirectories.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ReleaseSourceState.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ReleasePrivacy.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ReleaseRedistributable.psm1') -Force

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

function Assert-PrivateReleaseSidecar {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256
    )

    $archiveName = [IO.Path]::GetFileName($ArchivePath)
    $sidecarPath = "$ArchivePath.sha256"
    $expectedBytes = [Text.UTF8Encoding]::new($false).GetBytes(
        "$ExpectedSha256  $archiveName`n")
    $actualBytes = [IO.File]::ReadAllBytes($sidecarPath)
    if ([Convert]::ToHexString($actualBytes) -cne [Convert]::ToHexString($expectedBytes)) {
        throw "Private release sidecar is not strictly bound to its archive: $sidecarPath"
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$releaseSourceContract = Get-ReleaseSourceContract
Assert-ReleaseSourceState `
    -RepositoryRoot $repositoryRoot `
    -RequiredSubmodules $releaseSourceContract | Out-Null
$artifactsDirectory = Open-SafeReleaseRoot -Path (Join-Path $repositoryRoot 'artifacts')
$outputDirectory = $null
$releaseWorkDirectory = $null
try {
    $outputDirectory = Open-SafeReleaseRoot -Path $OutputPath
    $releaseWorkDirectory = New-SafeReleaseDirectory `
        -TrustedRoot $artifactsDirectory.Path `
        -LeafPrefix "release-work-$Version-"
    $releaseWork = $releaseWorkDirectory.Path
    $hostPublish = Join-Path $releaseWork 'rmm-bridge-host'
    $launcherPublish = Join-Path $releaseWork 'launcher'
    $releaseOutput = Join-Path $releaseWork 'release-output'
    [IO.Directory]::CreateDirectory($hostPublish) | Out-Null
    [IO.Directory]::CreateDirectory($launcherPublish) | Out-Null
    [IO.Directory]::CreateDirectory($releaseOutput) | Out-Null

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
            $releaseOutput) `
        -FailureMessage 'Verified release packaging failed.'

    Assert-ReleaseSourceState `
        -RepositoryRoot $repositoryRoot `
        -RequiredSubmodules $releaseSourceContract | Out-Null

    $sourcePackageScript = Join-Path $repositoryRoot 'packaging/build-source-release.ps1'
    Invoke-CheckedCommand `
        -FilePath 'pwsh' `
        -Arguments @(
            '-NoProfile',
            '-File',
            $sourcePackageScript,
            '-Version',
            $Version,
            '-OutputPath',
            $releaseOutput) `
        -FailureMessage 'Corresponding-source packaging failed.'

    $binaryName = "DSR-for-MOD-v$Version-win-x64.zip"
    $sourceName = "DSR-for-MOD-v$Version-source.zip"
    $privateArtifactNames = @(
        $binaryName,
        "$binaryName.sha256",
        $sourceName,
        "$sourceName.sha256"
    )
    foreach ($name in $privateArtifactNames) {
        $stagedPath = Join-Path $releaseOutput $name
        if (-not (Test-Path -LiteralPath $stagedPath -PathType Leaf)) {
            throw "The verified release pair is incomplete: $stagedPath"
        }
    }
    $binaryPath = Join-Path $releaseOutput $binaryName
    $sourcePath = Join-Path $releaseOutput $sourceName
    $prePrivacyArchiveHashes = [ordered]@{}
    foreach ($archiveName in @($binaryName, $sourceName)) {
        $prePrivacyArchiveHashes[$archiveName] = (Get-FileHash `
            -LiteralPath (Join-Path $releaseOutput $archiveName) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    Assert-PrivateReleaseSidecar `
        -ArchivePath $binaryPath `
        -ExpectedSha256 $prePrivacyArchiveHashes[$binaryName]
    Assert-PrivateReleaseSidecar `
        -ArchivePath $sourcePath `
        -ExpectedSha256 $prePrivacyArchiveHashes[$sourceName]
    Assert-ReleaseArchivePrivacy -ArchivePath $binaryPath
    Assert-ReleaseArchivePrivacy -ArchivePath $sourcePath
    $gatedArchiveHashes = [ordered]@{}
    foreach ($archiveName in @($binaryName, $sourceName)) {
        $gatedHash = (Get-FileHash `
            -LiteralPath (Join-Path $releaseOutput $archiveName) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($gatedHash -cne $prePrivacyArchiveHashes[$archiveName]) {
            throw "A release ZIP changed while its final privacy gate was running: $archiveName"
        }
        $gatedArchiveHashes[$archiveName] = $gatedHash
    }
    $redistributableName = "DSR-for-MOD-v$Version-redistributable.zip"
    $redistributablePath = Join-Path $releaseOutput $redistributableName
    New-ReleaseRedistributableArchive `
        -Version $Version `
        -SourceArchivePath $sourcePath `
        -BinaryArchivePath $binaryPath `
        -OutputPath $redistributablePath
    $redistributable = Assert-ReleaseRedistributableArchive `
        -ArchivePath $redistributablePath `
        -Version $Version
    if ($redistributable.SourceSha256 -cne $gatedArchiveHashes[$sourceName] `
            -or $redistributable.BinarySha256 -cne $gatedArchiveHashes[$binaryName]) {
        throw 'Redistributable inner archives do not match their final gated hashes.'
    }
    Assert-ReleaseArchivePrivacy -ArchivePath $redistributablePath
    $expectedRedistributableHash = (Get-FileHash `
        -LiteralPath $redistributablePath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Publish-ReleaseRedistributableArchive `
        -StagedArchivePath $redistributablePath `
        -OutputRoot $outputDirectory.Path `
        -FileName $redistributableName `
        -ExpectedSha256 $expectedRedistributableHash `
        -Version $Version
    Remove-LegacyReleaseArtifacts `
        -OutputRoot $outputDirectory.Path `
        -Names $privateArtifactNames
}
finally {
    try {
        if ($null -ne $releaseWorkDirectory) {
            Remove-SafeReleaseDirectory -Directory $releaseWorkDirectory
        }
    }
    finally {
        try {
            if ($null -ne $outputDirectory) {
                $outputDirectory.Lease.Dispose()
            }
        }
        finally {
            $artifactsDirectory.Lease.Dispose()
        }
    }
}
