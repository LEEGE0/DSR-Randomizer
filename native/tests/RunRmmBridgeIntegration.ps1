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
$deployedBridge = Join-Path $componentRoot 'DSRRandomizer.RmmBridge.dll'
$saveRoot = Join-Path $testRoot "saves\$steamId"
$gamePath = Join-Path $runtimeRoot 'DarkSoulsRemastered.exe'
$rmmPath = Join-Path $saveRoot 'DRAKS0005.rmm'
$overhaulSource = Join-Path $runtimeRoot 'overhaul\GameParam.parambnd.dcx'
$overhaulTarget = Join-Path $componentRoot 'content\overhaul\GameParam.parambnd.dcx'
$documentsPath = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
$normalSavePath = Join-Path $documentsPath "NBGI\DARK SOULS REMASTERED\$steamId\DRAKS0005.sl2"
$normalSaveExisted = Test-Path -LiteralPath $normalSavePath -PathType Leaf
$normalSaveHash = if ($normalSaveExisted) {
    (Get-FileHash -LiteralPath $normalSavePath -Algorithm SHA256).Hash
} else {
    $null
}

try {
    $virtualSaveRoot = Join-Path $testRoot "profile\NBGI\DARK SOULS REMASTERED\$steamId"
    New-Item -ItemType Directory -Path $runtimeRoot,$componentRoot,$saveRoot,(Join-Path $testRoot 'config'),$virtualSaveRoot,(Join-Path $testRoot 'logs'),(Split-Path -Parent $overhaulSource),(Split-Path -Parent $overhaulTarget) | Out-Null
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
    $start.ArgumentList.Add($deployedBridge)
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
    if (-not $normalSaveExisted -and (Test-Path -LiteralPath $normalSavePath -PathType Leaf)) {
        throw 'The normal Documents SL2 was created instead of redirecting to the RMM.'
    }
    if ($normalSaveExisted -and (Get-FileHash -LiteralPath $normalSavePath -Algorithm SHA256).Hash -cne $normalSaveHash) {
        throw 'The pre-existing normal Documents SL2 changed instead of redirecting to the RMM.'
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
