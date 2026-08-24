[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [Parameter(Mandatory = $true)]
    [string]$LauncherPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-InstallSnapshot {
    param([Parameter(Mandatory = $true)][string]$Root)

    $items = Get-ChildItem -LiteralPath $Root -Recurse -Force
    $reparsePoint = $items | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    } | Select-Object -First 1
    if ($null -ne $reparsePoint) {
        throw "Cannot prove immutability while a reparse point exists: $($reparsePoint.FullName)"
    }

    @($items | Where-Object { -not $_.PSIsContainer } | Sort-Object FullName | ForEach-Object {
        $relativePath = $_.FullName.Substring($Root.Length).TrimStart('\', '/').Replace('\', '/')
        [pscustomobject]@{
            relativePath = $relativePath
            length = $_.Length
            lastWriteTimeUtcTicks = $_.LastWriteTimeUtc.Ticks
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}

function Get-SnapshotIndex {
    param([Parameter(Mandatory = $true)][object[]]$Snapshot)

    $index = @{}
    foreach ($entry in $Snapshot) {
        $index[$entry.relativePath] = "$($entry.length)|$($entry.lastWriteTimeUtcTicks)|$($entry.sha256)"
    }
    $index
}

$gameRoot = (Resolve-Path -LiteralPath $GamePath).Path.TrimEnd('\', '/')
$launcher = (Resolve-Path -LiteralPath $LauncherPath).Path
$runningGame = Get-Process -Name 'DarkSoulsRemastered' -ErrorAction SilentlyContinue
if ($null -ne $runningGame) {
    throw 'DarkSoulsRemastered.exe is running. Close the original, Overhaul, and random game before the immutability proof.'
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifactRoot = Join-Path $repositoryRoot 'artifacts'
[IO.Directory]::CreateDirectory($artifactRoot) | Out-Null
$beforePath = Join-Path $artifactRoot 'original-install-before.json'
$afterPath = Join-Path $artifactRoot 'original-install-after.json'
$comparisonPath = Join-Path $artifactRoot 'original-install-comparison.json'

$before = Get-InstallSnapshot -Root $gameRoot
[IO.File]::WriteAllText(
    $beforePath,
    ($before | ConvertTo-Json -Depth 4),
    [Text.UTF8Encoding]::new($false))

$launcherOutput = & $launcher --initialize-runtime $gameRoot 2>&1 | Out-String
$launcherExitCode = $LASTEXITCODE
if ($launcherExitCode -ne 0) {
    throw "Runtime initialization failed with exit code $launcherExitCode. Output: $launcherOutput"
}

$after = Get-InstallSnapshot -Root $gameRoot
[IO.File]::WriteAllText(
    $afterPath,
    ($after | ConvertTo-Json -Depth 4),
    [Text.UTF8Encoding]::new($false))

$beforeIndex = Get-SnapshotIndex -Snapshot $before
$afterIndex = Get-SnapshotIndex -Snapshot $after
$allPaths = @($beforeIndex.Keys + $afterIndex.Keys | Sort-Object -Unique)
$changes = @($allPaths | Where-Object {
    -not $beforeIndex.ContainsKey($_) -or
    -not $afterIndex.ContainsKey($_) -or
    $beforeIndex[$_] -cne $afterIndex[$_]
})
$result = [pscustomobject]@{
    changedFiles = $changes.Count
    changedPaths = $changes
    beforeSnapshot = $beforePath
    afterSnapshot = $afterPath
}
[IO.File]::WriteAllText(
    $comparisonPath,
    ($result | ConvertTo-Json -Depth 4),
    [Text.UTF8Encoding]::new($false))
$result | ConvertTo-Json -Compress

if ($changes.Count -ne 0) {
    exit 1
}
