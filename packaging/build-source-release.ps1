[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $false)]
    [string]$SourceRevision = 'HEAD'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'SafeReleaseDirectories.psm1') -Force
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Invoke-GitCapture {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    $output = & git -C $WorkingDirectory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage Exit code: $LASTEXITCODE`n$($output -join [Environment]::NewLine)"
    }
    return ($output -join [Environment]::NewLine).Trim()
}

function Invoke-GitArchive {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Revision,
        [Parameter(Mandatory = $true)][string]$OutputFile
    )

    & git -C $WorkingDirectory archive --format=zip --output=$OutputFile $Revision
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to archive committed source revision '$Revision'. Exit code: $LASTEXITCODE"
    }
}

function Test-ProhibitedSourcePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $segments = $Path -split '/'
    foreach ($segment in $segments) {
        if ($segment -in @('.git', 'bin', 'obj', 'artifacts', '.superpowers', 'private', 'generated')) {
            return $true
        }
    }
    return $false
}

function Add-ArchiveFiles {
    param(
        [Parameter(Mandatory = $true)][Collections.Generic.SortedDictionary[string, byte[]]]$Files,
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$DestinationPrefix
    )

    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName.EndsWith('/', [StringComparison]::Ordinal)) {
                continue
            }

            $relativePath = $entry.FullName.Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($relativePath) `
                    -or [IO.Path]::IsPathRooted($relativePath) `
                    -or ($relativePath -split '/') -contains '..' `
                    -or (Test-ProhibitedSourcePath -Path $relativePath)) {
                throw "Committed source contains a prohibited archive path: $relativePath"
            }

            $destinationPath = "$DestinationPrefix$relativePath"
            if ($Files.ContainsKey($destinationPath)) {
                throw "Committed source contains a duplicate archive path: $destinationPath"
            }

            $stream = $entry.Open()
            try {
                $memory = [IO.MemoryStream]::new()
                try {
                    $stream.CopyTo($memory)
                    $Files.Add($destinationPath, $memory.ToArray())
                }
                finally {
                    $memory.Dispose()
                }
            }
            finally {
                $stream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$submoduleRoot = Join-Path $repositoryRoot 'third_party/SoulsFormatsNEXT'
$artifactsDirectory = Open-SafeReleaseRoot -Path (Join-Path $repositoryRoot 'artifacts')
$outputDirectory = $null
$workDirectory = $null
try {
    $outputDirectory = Open-SafeReleaseRoot -Path $OutputPath
    $workDirectory = New-SafeReleaseDirectory `
        -TrustedRoot $artifactsDirectory.Path `
        -LeafPrefix "source-release-work-$Version-"

    $resolvedRevision = Invoke-GitCapture `
        -WorkingDirectory $repositoryRoot `
        -Arguments @('rev-parse', '--verify', "$SourceRevision^{commit}") `
        -FailureMessage "Source revision '$SourceRevision' is not a committed tree."
    $submoduleRevision = Invoke-GitCapture `
        -WorkingDirectory $repositoryRoot `
        -Arguments @('rev-parse', "$resolvedRevision`:third_party/SoulsFormatsNEXT") `
        -FailureMessage 'The source revision does not pin SoulsFormatsNEXT.'
    $submoduleTreeEntry = Invoke-GitCapture `
        -WorkingDirectory $repositoryRoot `
        -Arguments @('ls-tree', $resolvedRevision, 'third_party/SoulsFormatsNEXT') `
        -FailureMessage 'Unable to inspect the SoulsFormatsNEXT gitlink.'
    if (-not $submoduleTreeEntry.StartsWith(
            "160000 commit $submoduleRevision`t",
            [StringComparison]::Ordinal)) {
        throw "SoulsFormatsNEXT is not pinned as a gitlink at source revision $resolvedRevision."
    }
    if (-not (Test-Path -LiteralPath $submoduleRoot -PathType Container)) {
        throw "The pinned SoulsFormatsNEXT checkout is missing: $submoduleRoot"
    }
    Invoke-GitCapture `
        -WorkingDirectory $submoduleRoot `
        -Arguments @('cat-file', '-e', "$submoduleRevision^{commit}") `
        -FailureMessage "The pinned SoulsFormatsNEXT commit is unavailable: $submoduleRevision" | Out-Null

    $mainArchive = Join-Path $workDirectory.Path 'main.zip'
    $submoduleArchive = Join-Path $workDirectory.Path 'soulsformats.zip'
    Invoke-GitArchive -WorkingDirectory $repositoryRoot -Revision $resolvedRevision -OutputFile $mainArchive
    Invoke-GitArchive -WorkingDirectory $submoduleRoot -Revision $submoduleRevision -OutputFile $submoduleArchive

    $rootPrefix = "DSR-for-MOD-v$Version-source/"
    $files = [Collections.Generic.SortedDictionary[string, byte[]]]::new(
        [StringComparer]::Ordinal)
    Add-ArchiveFiles -Files $files -ArchivePath $mainArchive -DestinationPrefix $rootPrefix
    Add-ArchiveFiles `
        -Files $files `
        -ArchivePath $submoduleArchive `
        -DestinationPrefix "${rootPrefix}third_party/SoulsFormatsNEXT/"

    $requiredPaths = @(
        "${rootPrefix}DSR-Randomizer.sln",
        "${rootPrefix}LICENSE",
        "${rootPrefix}packaging/build-source-release.ps1",
        "${rootPrefix}src/DSRRandomizer.SoulsFormatsSubset/DSRRandomizer.SoulsFormatsSubset.csproj",
        "${rootPrefix}third_party/SoulsFormatsNEXT/LICENSE",
        "${rootPrefix}third_party/SoulsFormatsNEXT/SoulsFormats/SoulsFormats.csproj"
    )
    foreach ($requiredPath in $requiredPaths) {
        if (-not $files.ContainsKey($requiredPath)) {
            throw "The committed corresponding-source tree is incomplete: $requiredPath"
        }
    }

    $archiveName = "DSR-for-MOD-v$Version-source.zip"
    $temporaryArchive = Join-Path $workDirectory.Path $archiveName
    $archive = [IO.Compression.ZipFile]::Open(
        $temporaryArchive,
        [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in $files.GetEnumerator()) {
            $entry = $archive.CreateEntry($item.Key, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
            $stream = $entry.Open()
            try {
                $stream.Write($item.Value, 0, $item.Value.Length)
            }
            finally {
                $stream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }

    $hash = (Get-FileHash -LiteralPath $temporaryArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    $temporaryChecksum = "$temporaryArchive.sha256"
    [IO.File]::WriteAllText(
        $temporaryChecksum,
        "$hash  $archiveName`n",
        [Text.UTF8Encoding]::new($false))

    $finalArchive = Join-Path $outputDirectory.Path $archiveName
    $finalChecksum = "$finalArchive.sha256"
    [IO.File]::Move($temporaryArchive, $finalArchive, $true)
    [IO.File]::Move($temporaryChecksum, $finalChecksum, $true)

    Write-Output "Source revision: $resolvedRevision"
    Write-Output "SoulsFormatsNEXT revision: $submoduleRevision"
    Write-Output "Corresponding-source archive: $finalArchive"
    Write-Output "Corresponding-source SHA-256: $hash"
}
finally {
    try {
        if ($null -ne $workDirectory) {
            Remove-SafeReleaseDirectory -Directory $workDirectory
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
