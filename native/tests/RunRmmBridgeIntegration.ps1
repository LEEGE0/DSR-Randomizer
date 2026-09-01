param(
    [Parameter(Mandatory = $true)][string]$LoaderFixture,
    [Parameter(Mandatory = $true)][string]$HostFixture,
    [Parameter(Mandatory = $true)][string]$BridgeDll
)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("dsr-rmm-bridge-" + [Guid]::NewGuid().ToString('N'))
$runtimeId = 'runtime-0123456789abcdef'
$steamId = '424242424'
$runtimeRoot = Join-Path $testRoot "runtimes\$runtimeId"
$componentRoot = Join-Path $testRoot 'components\rmm-bridge'
$deployedBridge = Join-Path $componentRoot 'DSRRandomizer.RmmBridge.dll'
$saveRoot = Join-Path $testRoot "saves\$steamId"
$gamePath = Join-Path $runtimeRoot 'DarkSoulsRemastered.exe'
$rmmPath = Join-Path $saveRoot 'DRAKS0005.rmm'
$overhaulSource = Join-Path $runtimeRoot 'overhaul\GameParam.parambnd.dcx'
$overhaulTarget = Join-Path $componentRoot 'content\overhaul\GameParam.parambnd.dcx'
$safeLogicalSave = Join-Path $testRoot "logical\NBGI\DARK SOULS REMASTERED\$steamId\DRAKS0005.sl2"

try {
    New-Item -ItemType Directory -Path $runtimeRoot,$componentRoot,$saveRoot,(Join-Path $testRoot 'config'),(Join-Path $testRoot 'logs'),(Split-Path -Parent $safeLogicalSave),(Split-Path -Parent $overhaulSource),(Split-Path -Parent $overhaulTarget) | Out-Null
    Copy-Item -LiteralPath $LoaderFixture -Destination $gamePath
    Copy-Item -LiteralPath $HostFixture -Destination (Join-Path $componentRoot 'DSRRandomizer.RmmBridgeHost.exe')
    Copy-Item -LiteralPath $BridgeDll -Destination $deployedBridge
    [IO.File]::WriteAllBytes($overhaulSource, [byte[]](0x53, 0x52, 0x43))
    [IO.File]::WriteAllBytes($overhaulTarget, [byte[]](0x47, 0x45, 0x4e))

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
            sourcePath = $safeLogicalSave
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
    $start.ArgumentList.Add($deployedBridge)
    $start.ArgumentList.Add($safeLogicalSave)
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
    if (Test-Path -LiteralPath $safeLogicalSave -PathType Leaf) {
        throw 'The safe logical SL2 was created instead of redirecting to the RMM.'
    }
    $log = [IO.File]::ReadAllText((Join-Path $testRoot 'logs\rmm-bridge.log'))
    if (-not $log.Contains('DIAGNOSTIC host-ready', [StringComparison]::Ordinal) `
        -or -not $log.Contains('DIAGNOSTIC callsite-installed', [StringComparison]::Ordinal)) {
        throw 'The bridge did not report host readiness and callsite installation.'
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
