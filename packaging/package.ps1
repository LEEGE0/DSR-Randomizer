[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z.-]*$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$PublishPath,

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
        throw "Package path escaped the output root: $normalizedCandidate"
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
        throw "Runtime package is missing from the publish dependency manifest: $PackageName"
    }
    $library.Substring('runtimepack.'.Length).Split('/', 2)
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$publishRoot = (Resolve-Path -LiteralPath $PublishPath).Path
$outputRoot = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$stagingRoot = Join-Path $outputRoot "package-staging-$Version"
$zipName = "DSR-Randomizer-v$Version-win-x64.zip"
$zipPath = Join-Path $outputRoot $zipName
$checksumPath = "$zipPath.sha256"
Assert-StrictDescendant -Candidate $stagingRoot -Root $outputRoot
Assert-StrictDescendant -Candidate $zipPath -Root $outputRoot
Assert-StrictDescendant -Candidate $checksumPath -Root $outputRoot

$publishedLauncher = Join-Path $publishRoot 'DSRRandomizer.Launcher.exe'
if (-not (Test-Path -LiteralPath $publishedLauncher -PathType Leaf)) {
    throw "Published launcher is missing: $publishedLauncher"
}

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
[IO.Directory]::CreateDirectory($stagingRoot) | Out-Null

try {
    Copy-Item -LiteralPath $publishedLauncher -Destination $stagingRoot
    $publishedPdb = Join-Path $publishRoot 'DSRRandomizer.Launcher.pdb'
    if (Test-Path -LiteralPath $publishedPdb -PathType Leaf) {
        Copy-Item -LiteralPath $publishedPdb -Destination $stagingRoot
    }
    foreach ($notice in @('README.md', 'LICENSE', 'CHANGELOG.md')) {
        Copy-Item -LiteralPath (Join-Path $repositoryRoot $notice) -Destination $stagingRoot
    }
    $publishedGuard = Join-Path $publishRoot 'native/DSRRandomizer.Runtime.dll'
    $publishedProfile = Join-Path $publishRoot 'config/compatibility-profiles.json'
    if (-not (Test-Path -LiteralPath $publishedGuard -PathType Leaf)) {
        throw "Published native guard is missing: $publishedGuard"
    }
    if (-not (Test-Path -LiteralPath $publishedProfile -PathType Leaf)) {
        throw "Published compatibility profile is missing: $publishedProfile"
    }
    $stagedNative = Join-Path $stagingRoot 'native'
    $stagedConfig = Join-Path $stagingRoot 'config'
    [IO.Directory]::CreateDirectory($stagedNative) | Out-Null
    [IO.Directory]::CreateDirectory($stagedConfig) | Out-Null
    $stagedGuard = Join-Path $stagedNative 'DSRRandomizer.Runtime.dll'
    Copy-Item -LiteralPath $publishedGuard -Destination $stagedGuard
    Copy-Item -LiteralPath $publishedProfile -Destination $stagedConfig
    $guardHash = (Get-FileHash -LiteralPath $stagedGuard -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        "$stagedGuard.sha256",
        "$guardHash`n",
        [Text.UTF8Encoding]::new($false))

    $dependencyManifest = Get-ChildItem `
        -LiteralPath (Join-Path $repositoryRoot 'src/DSRRandomizer.Launcher/obj/Release') `
        -Filter 'DSRRandomizer.Launcher.deps.json' `
        -File `
        -Recurse |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -eq $dependencyManifest) {
        throw 'The publish dependency manifest was not generated.'
    }
    $dependencies = Get-Content -LiteralPath $dependencyManifest.FullName -Raw | ConvertFrom-Json
    $corePackage = Get-RuntimePackage -Dependencies $dependencies -PackageName 'Microsoft.NETCore.App.Runtime.win-x64'
    $desktopPackage = Get-RuntimePackage -Dependencies $dependencies -PackageName 'Microsoft.WindowsDesktop.App.Runtime.win-x64'
    $nugetRoot = if ([string]::IsNullOrWhiteSpace($env:NUGET_PACKAGES)) {
        Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)) '.nuget/packages'
    } else {
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

    $stagedLauncher = Join-Path $stagingRoot 'DSRRandomizer.Launcher.exe'
    Invoke-PackageValidator -Launcher $stagedLauncher -Directory $stagingRoot

    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Add-Type -AssemblyName System.IO.Compression
    $zipStream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $zipStream,
            [IO.Compression.ZipArchiveMode]::Create,
            $true)
        try {
            foreach ($file in Get-ChildItem -LiteralPath $stagingRoot -File -Recurse |
                     Sort-Object FullName) {
                $entryName = [IO.Path]::GetRelativePath($stagingRoot, $file.FullName).Replace('\', '/')
                $entry = $archive.CreateEntry($entryName, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $input = [IO.File]::OpenRead($file.FullName)
                $output = $entry.Open()
                try {
                    $input.CopyTo($output)
                } finally {
                    $output.Dispose()
                    $input.Dispose()
                }
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $zipStream.Dispose()
    }

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
} finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
