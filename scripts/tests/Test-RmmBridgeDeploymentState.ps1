$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$modulePath = Join-Path $PSScriptRoot '..\RmmBridgeDeploymentState.psm1'
Import-Module $modulePath -Force

function Assert-Throws {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Name
    )

    try {
        & $Action
    }
    catch {
        return
    }
    throw "$Name did not reject the invalid deployment state."
}

function Write-Metadata {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$CleanExit
    )

    $metadata = @{
        schemaVersion = 1
        steamId = '100000001'
        fixedLength = 4326608
        lastKnownSha256 = ('0' * 64)
        cleanExit = $CleanExit
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($Path, $metadata)
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'dsr-rmm-deployment-state-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $rmmPath = Join-Path $testRoot 'DRAKS0005.rmm'
    $metadataPath = Join-Path $testRoot 'save-metadata.json'
    $stream = [IO.File]::Open($rmmPath, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.SetLength(4326608)
    }
    finally {
        $stream.Dispose()
    }

    # Production break: rejecting a recoverable unclean save solely because
    # its stored SHA-256 predates its final write.
    Write-Metadata -Path $metadataPath -CleanExit $false
    $recovered = Resolve-RmmBridgeDeploymentSaveState `
        -RmmPath $rmmPath -MetadataPath $metadataPath -SteamId '100000001'
    if ($recovered.CleanExit -or $recovered.LastKnownSha256 -cne ('0' * 64) `
        -or $recovered.ActualSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'An unclean save did not return its stored state and actual SHA-256.'
    }

    # Production break: accepting a clean save whose contents no longer match
    # the metadata identity recorded at the last clean exit.
    Write-Metadata -Path $metadataPath -CleanExit $true
    Assert-Throws -Name 'clean stale hash' -Action {
        Resolve-RmmBridgeDeploymentSaveState `
            -RmmPath $rmmPath -MetadataPath $metadataPath -SteamId '100000001'
    }

    # Production break: treating the string "false" as a JSON boolean and
    # accidentally bypassing the clean-save identity check through truthiness.
    Write-Metadata -Path $metadataPath -CleanExit 'false'
    Assert-Throws -Name 'string cleanExit' -Action {
        Resolve-RmmBridgeDeploymentSaveState `
            -RmmPath $rmmPath -MetadataPath $metadataPath -SteamId '100000001'
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
