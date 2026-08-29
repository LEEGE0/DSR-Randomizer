param(
    [Parameter(Mandatory = $true)][string]$LoaderFixture,
    [Parameter(Mandatory = $true)][string]$HostFixture,
    [Parameter(Mandatory = $true)][string]$BridgeDll
)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("dsr-rmm-bridge-" + [Guid]::NewGuid().ToString('N'))
$runtimeId = 'runtime-0123456789abcdef'
$steamId = '146808034'
$runtimeRoot = Join-Path $testRoot "runtimes\$runtimeId"
$componentRoot = Join-Path $testRoot 'components\rmm-bridge'
$saveRoot = Join-Path $testRoot "saves\$steamId"
$gamePath = Join-Path $runtimeRoot 'DarkSoulsRemastered.exe'
$rmmPath = Join-Path $saveRoot 'DRAKS0005.rmm'

try {
    $virtualSaveRoot = Join-Path $testRoot "profile\NBGI\DARK SOULS REMASTERED\$steamId"
    New-Item -ItemType Directory -Path $runtimeRoot,$componentRoot,$saveRoot,(Join-Path $testRoot 'config'),$virtualSaveRoot,(Join-Path $testRoot 'logs') | Out-Null
    Copy-Item -LiteralPath $LoaderFixture -Destination $gamePath
    Copy-Item -LiteralPath $HostFixture -Destination (Join-Path $componentRoot 'DSRRandomizer.RmmBridgeHost.exe')

    $stream = [IO.File]::Open($rmmPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.SetLength(4326608)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
    $rmmHash = (Get-FileHash -LiteralPath $rmmPath -Algorithm SHA256).Hash.ToLowerInvariant()

    [IO.File]::WriteAllText(
        (Join-Path $testRoot 'runtime-current.json'),
        (@{
            runtimeId = $runtimeId
            relativeRuntimePath = "runtimes/$runtimeId"
            manifestSha256 = ('a' * 64)
        } | ConvertTo-Json -Compress))
    [IO.File]::WriteAllText(
        (Join-Path $testRoot 'config\selected-save-profile.json'),
        (@{
            steamId = $steamId
            sourcePath = "C:\Normal\$steamId\DRAKS0005.sl2"
        } | ConvertTo-Json -Compress))
    [IO.File]::WriteAllText(
        (Join-Path $saveRoot 'save-metadata.json'),
        (@{
            schemaVersion = 1
            steamId = $steamId
            fixedLength = 4326608
            lastKnownSha256 = $rmmHash
            activeSeedId = $null
            placementSha256 = $null
            cleanExit = $true
        } | ConvertTo-Json -Compress))

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $gamePath
    $start.UseShellExecute = $false
    $start.ArgumentList.Add($BridgeDll)
    $process = [Diagnostics.Process]::Start($start)
    if (-not $process.WaitForExit(30000)) {
        $process.Kill($true)
        throw 'The synthetic Mod Engine loader timed out.'
    }
    if ($process.ExitCode -ne 0) {
        $logPath = Join-Path $testRoot 'logs\rmm-bridge.log'
        $log = if (Test-Path -LiteralPath $logPath) {
            [IO.File]::ReadAllText($logPath)
        } else {
            '<no bridge log>'
        }
        throw "The synthetic loader exited with code $($process.ExitCode). $log"
    }
    $stream = [IO.File]::OpenRead($rmmPath)
    try {
        $stream.Position = 128
        if ($stream.ReadByte() -ne 0x5a) {
            throw 'The dedicated RMM marker was not written.'
        }
    }
    finally {
        $stream.Dispose()
    }
    if (Test-Path -LiteralPath (Join-Path $testRoot "profile\NBGI\DARK SOULS REMASTERED\$steamId\DRAKS0005.sl2")) {
        throw 'A virtual SL2 was created instead of redirecting to the RMM.'
    }
}
finally {
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $insideTemp = $resolvedRoot.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase)
    $safeLeaf = [IO.Path]::GetFileName($resolvedRoot).StartsWith(
        'dsr-rmm-bridge-', [StringComparison]::Ordinal)
    if ($insideTemp -and $safeLeaf) {
        for ($attempt = 0; $attempt -lt 20; $attempt++) {
            try {
                if (Test-Path -LiteralPath $resolvedRoot) {
                    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
                }
                break
            }
            catch {
                Start-Sleep -Milliseconds 100
            }
        }
    }
}
