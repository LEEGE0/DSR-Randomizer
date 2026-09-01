[CmdletBinding(DefaultParameterSetName = 'Package')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Package')]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$')]
    [string]$Version,

    [Parameter(Mandatory = $true, ParameterSetName = 'Package')]
    [string]$PublishPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Package')]
    [string]$DependencyManifestPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Package')]
    [string]$OutputPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'ValidateArchive')]
    [string]$ValidateArchivePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'SafeReleaseDirectories.psm1') -Force

$ExpectedPackagePaths = @(
    'CHANGELOG.md',
    'DSRForMod.Launcher.exe',
    'INSTALL_KO.md',
    'LICENSE',
    'README.md',
    'THIRD_PARTY_NOTICES.md',
    'components/rmm-bridge/DSRRandomizer.RmmBridge.dll',
    'components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe',
    'components/rmm-bridge/deployment-manifest.json',
    'config/compatibility-profiles.json',
    'native/DSRRandomizer.Runtime.dll',
    'native/DSRRandomizer.Runtime.dll.sha256'
)

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
        throw "Package path escaped the output root: $normalizedCandidate"
    }
}

function Assert-ExactFileSet {
    param([Parameter(Mandatory = $true)][string]$Root)

    $actual = @(Get-ChildItem -LiteralPath $Root -File -Recurse | ForEach-Object {
        [IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/')
    })
    $actualSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($path in $actual) {
        if (-not $actualSet.Add($path)) {
            throw "The staged package contains a duplicate path: $path"
        }
    }
    $expectedSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($path in $ExpectedPackagePaths) {
        $expectedSet.Add($path) | Out-Null
    }
    if (-not $actualSet.SetEquals($expectedSet)) {
        $unexpected = @($actual | Where-Object { -not $expectedSet.Contains($_) })
        $missing = @($ExpectedPackagePaths | Where-Object { -not $actualSet.Contains($_) })
        throw "The staged package does not match the exact allowlist. Unexpected=[$($unexpected -join ',')] Missing=[$($missing -join ',')]"
    }
}

function Assert-ReleaseArchiveEntries {
    param([Parameter(Mandatory = $true)][string]$ArchivePath)

    Add-Type -AssemblyName System.IO.Compression
    $resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
    $stream = [IO.File]::Open(
        $resolvedArchive,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $stream,
            [IO.Compression.ZipArchiveMode]::Read,
            $true)
        try {
            $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            $names = [Collections.Generic.List[string]]::new()
            foreach ($entry in $archive.Entries) {
                $entryName = [string]$entry.FullName
                $segments = $entryName.Split(
                    [char[]]@('/'),
                    [StringSplitOptions]::None)
                $hasUnsafeSegment = @($segments | Where-Object {
                    [string]::IsNullOrEmpty($_) -or $_ -ceq '.' -or $_ -ceq '..'
                }).Count -ne 0
                $unsafe = [string]::IsNullOrWhiteSpace($entryName) `
                    -or $entryName.Contains('\', [StringComparison]::Ordinal) `
                    -or $entryName.StartsWith('/', [StringComparison]::Ordinal) `
                    -or [IO.Path]::IsPathRooted($entryName) `
                    -or $hasUnsafeSegment
                if ($unsafe -or -not $seen.Add($entryName)) {
                    throw "Release archive entry validation failed: $entryName"
                }
                $names.Add($entryName)
            }

            if ($names.Count -ne $ExpectedPackagePaths.Count) {
                throw 'Release archive entry validation failed: entry count does not match the allowlist.'
            }
            for ($index = 0; $index -lt $ExpectedPackagePaths.Count; $index++) {
                if ($names[$index] -cne $ExpectedPackagePaths[$index]) {
                    throw "Release archive entry validation failed: unexpected or unsorted entry $($names[$index])"
                }
            }
        }
        finally {
            $archive.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Invoke-PackageValidator {
    param(
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)][string]$Directory
    )

    $quotedDirectory = '"' + $Directory.Replace('"', '\"') + '"'
    $process = Start-Process `
        -FilePath $Launcher `
        -ArgumentList @('--validate-package', $quotedDirectory) `
        -Wait `
        -PassThru `
        -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "Release-content validation failed with exit code $($process.ExitCode): $Directory"
    }
}

function Get-RuntimePackage {
    param(
        [Parameter(Mandatory = $true)][object]$Dependencies,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    $library = $Dependencies.libraries.PSObject.Properties.Name |
        Where-Object { $_ -like "runtimepack.$PackageName/*" } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($library)) {
        throw "Runtime package is missing from the supplied dependency manifest: $PackageName"
    }
    $library.Substring('runtimepack.'.Length).Split('/', 2)
}

if ($PSCmdlet.ParameterSetName -eq 'ValidateArchive') {
    Assert-ReleaseArchiveEntries -ArchivePath $ValidateArchivePath
    Write-Output "Validated release archive entries: $ValidateArchivePath"
    return
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$publishRoot = (Resolve-Path -LiteralPath $PublishPath).Path
$dependencyManifest = (Resolve-Path -LiteralPath $DependencyManifestPath).Path
$outputDirectory = Open-SafeReleaseRoot -Path $OutputPath
$outputRoot = $outputDirectory.Path
$stagingDirectory = $null
$extractedDirectory = $null
try {
    $stagingDirectory = New-SafeReleaseDirectory `
        -TrustedRoot $outputRoot `
        -LeafPrefix "package-staging-$Version-"
    $extractedDirectory = New-SafeReleaseDirectory `
        -TrustedRoot $outputRoot `
        -LeafPrefix "package-extracted-$Version-"
    $stagingRoot = $stagingDirectory.Path
    $extractedRoot = $extractedDirectory.Path
    $zipName = "DSR-for-MOD-v$Version-win-x64.zip"
    $zipPath = Join-Path $outputRoot $zipName
    $checksumPath = "$zipPath.sha256"
Assert-StrictDescendant -Candidate $stagingRoot -Root $outputRoot
Assert-StrictDescendant -Candidate $extractedRoot -Root $outputRoot
Assert-StrictDescendant -Candidate $zipPath -Root $outputRoot
Assert-StrictDescendant -Candidate $checksumPath -Root $outputRoot
if ([IO.Path]::GetFileName($zipPath) -cne $zipName `
    -or [IO.Path]::GetFileName($checksumPath) -cne "$zipName.sha256") {
    throw 'The release output names are not the validated filenames.'
}

$publishedLauncher = Join-Path $publishRoot 'DSRForMod.Launcher.exe'
$publishedGuard = Join-Path $publishRoot 'native/DSRRandomizer.Runtime.dll'
$publishedProfile = Join-Path $publishRoot 'config/compatibility-profiles.json'
$publishedBridge = Join-Path $publishRoot 'components/rmm-bridge/DSRRandomizer.RmmBridge.dll'
$publishedHost = Join-Path $publishRoot 'components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe'
foreach ($path in @(
    $publishedLauncher,
    $publishedGuard,
    $publishedProfile,
    $publishedBridge,
    $publishedHost,
    $dependencyManifest)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "A required publish input is missing: $path"
    }
}

    Copy-Item -LiteralPath $publishedLauncher -Destination $stagingRoot
    foreach ($notice in @('README.md', 'INSTALL_KO.md', 'LICENSE', 'CHANGELOG.md')) {
        $source = Join-Path $repositoryRoot $notice
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "A required release document is missing: $source"
        }
        Copy-Item -LiteralPath $source -Destination $stagingRoot
    }

    $stagedNative = Join-Path $stagingRoot 'native'
    $stagedConfig = Join-Path $stagingRoot 'config'
    $stagedBridgeRoot = Join-Path $stagingRoot 'components/rmm-bridge'
    [IO.Directory]::CreateDirectory($stagedNative) | Out-Null
    [IO.Directory]::CreateDirectory($stagedConfig) | Out-Null
    [IO.Directory]::CreateDirectory($stagedBridgeRoot) | Out-Null

    $stagedGuard = Join-Path $stagedNative 'DSRRandomizer.Runtime.dll'
    $stagedBridge = Join-Path $stagedBridgeRoot 'DSRRandomizer.RmmBridge.dll'
    $stagedHost = Join-Path $stagedBridgeRoot 'DSRRandomizer.RmmBridgeHost.exe'
    Copy-Item -LiteralPath $publishedGuard -Destination $stagedGuard
    Copy-Item -LiteralPath $publishedProfile -Destination $stagedConfig
    Copy-Item -LiteralPath $publishedBridge -Destination $stagedBridge
    Copy-Item -LiteralPath $publishedHost -Destination $stagedHost

    $guardHash = (Get-FileHash -LiteralPath $stagedGuard -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        "$stagedGuard.sha256",
        "$guardHash`n",
        [Text.UTF8Encoding]::new($false))
    $bridgeHash = (Get-FileHash -LiteralPath $stagedBridge -Algorithm SHA256).Hash.ToLowerInvariant()
    $hostHash = (Get-FileHash -LiteralPath $stagedHost -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        schemaVersion = 1
        configuration = 'Release'
        bridgeSha256 = $bridgeHash
        hostSha256 = $hostHash
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText(
        (Join-Path $stagedBridgeRoot 'deployment-manifest.json'),
        $manifest,
        [Text.UTF8Encoding]::new($false))

    $dependencies = Get-Content -LiteralPath $dependencyManifest -Raw | ConvertFrom-Json
    $corePackage = @(Get-RuntimePackage `
        -Dependencies $dependencies `
        -PackageName 'Microsoft.NETCore.App.Runtime.win-x64')
    $desktopPackage = @(Get-RuntimePackage `
        -Dependencies $dependencies `
        -PackageName 'Microsoft.WindowsDesktop.App.Runtime.win-x64')
    $nugetRoot = if ([string]::IsNullOrWhiteSpace($env:NUGET_PACKAGES)) {
        Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)) '.nuget/packages'
    }
    else {
        [IO.Path]::GetFullPath($env:NUGET_PACKAGES)
    }
    $coreRoot = Join-Path $nugetRoot ("{0}/{1}" -f $corePackage[0].ToLowerInvariant(), $corePackage[1])
    $desktopRoot = Join-Path $nugetRoot ("{0}/{1}" -f $desktopPackage[0].ToLowerInvariant(), $desktopPackage[1])
    $noticeParts = @(
        (Get-Content -LiteralPath (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md') -Raw),
        "`n## Bundled .NET runtime package`n`nPackage: $($corePackage -join ' ')`n",
        (Get-Content -LiteralPath (Join-Path $coreRoot 'LICENSE.TXT') -Raw),
        "`n## Bundled Windows Desktop runtime package`n`nPackage: $($desktopPackage -join ' ')`n",
        (Get-Content -LiteralPath (Join-Path $desktopRoot 'LICENSE') -Raw),
        "`n## Exact .NET runtime third-party notices`n",
        (Get-Content -LiteralPath (Join-Path $coreRoot 'THIRD-PARTY-NOTICES.TXT') -Raw)
    )
    [IO.File]::WriteAllText(
        (Join-Path $stagingRoot 'THIRD_PARTY_NOTICES.md'),
        ($noticeParts -join "`n"),
        [Text.UTF8Encoding]::new($false))

    Assert-ExactFileSet -Root $stagingRoot
    $stagedLauncher = Join-Path $stagingRoot 'DSRForMod.Launcher.exe'
    Invoke-PackageValidator -Launcher $stagedLauncher -Directory $stagingRoot

    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    if (Test-Path -LiteralPath $checksumPath) {
        Remove-Item -LiteralPath $checksumPath -Force
    }

    Add-Type -AssemblyName System.IO.Compression
    $zipStream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $zipStream,
            [IO.Compression.ZipArchiveMode]::Create,
            $true)
        try {
            foreach ($entryName in $ExpectedPackagePaths) {
                $file = Join-Path $stagingRoot $entryName
                $entry = $archive.CreateEntry(
                    $entryName,
                    [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(
                    1980,
                    1,
                    1,
                    0,
                    0,
                    0,
                    [TimeSpan]::Zero)
                $input = [IO.File]::OpenRead($file)
                $output = $entry.Open()
                try {
                    $input.CopyTo($output)
                }
                finally {
                    $output.Dispose()
                    $input.Dispose()
                }
            }
        }
        finally {
            $archive.Dispose()
        }
    }
    finally {
        $zipStream.Dispose()
    }

    Assert-ReleaseArchiveEntries -ArchivePath $zipPath
    [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $extractedRoot)
    Assert-ExactFileSet -Root $extractedRoot
    Invoke-PackageValidator `
        -Launcher (Join-Path $extractedRoot 'DSRForMod.Launcher.exe') `
        -Directory $extractedRoot

    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        $checksumPath,
        "$hash  $zipName`n",
        [Text.UTF8Encoding]::new($false))
    [pscustomobject]@{
        zip = $zipPath
        checksum = $checksumPath
        sha256 = $hash
    } | ConvertTo-Json -Compress
}
finally {
    try {
        if ($null -ne $stagingDirectory) {
            Remove-SafeReleaseDirectory -Directory $stagingDirectory
        }
    }
    finally {
        try {
            if ($null -ne $extractedDirectory) {
                Remove-SafeReleaseDirectory -Directory $extractedDirectory
            }
        }
        finally {
            $outputDirectory.Lease.Dispose()
        }
    }
}
