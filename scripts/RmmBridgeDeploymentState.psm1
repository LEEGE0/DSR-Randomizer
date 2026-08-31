Set-StrictMode -Version Latest

function Read-StrictJson {
    param([Parameter(Mandatory = $true)][string]$Path)

    $information = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($information.Length -gt 65536) {
        throw "Configuration is oversized: $Path"
    }
    return [IO.File]::ReadAllText($Path) | ConvertFrom-Json
}

function Read-SaveMetadataState {
    param([Parameter(Mandatory = $true)][string]$Path)

    $metadata = Read-StrictJson $Path
    $cleanExitProperty = $metadata.PSObject.Properties['cleanExit']
    if ($null -eq $cleanExitProperty -or $cleanExitProperty.Value -isnot [bool]) {
        throw 'save-metadata.json cleanExit must be a JSON boolean.'
    }
    [pscustomobject]@{
        SchemaVersion = $metadata.schemaVersion
        SteamId = [string]$metadata.steamId
        FixedLength = $metadata.fixedLength
        LastKnownSha256 = [string]$metadata.lastKnownSha256
        CleanExit = [bool]$cleanExitProperty.Value
    }
}

function Resolve-RmmBridgeDeploymentSaveState {
    param(
        [Parameter(Mandatory = $true)][string]$RmmPath,
        [Parameter(Mandatory = $true)][string]$MetadataPath,
        [Parameter(Mandatory = $true)][string]$SteamId
    )

    $metadata = Read-SaveMetadataState $MetadataPath
    $rmm = Get-Item -LiteralPath $RmmPath -ErrorAction Stop
    if ($metadata.SchemaVersion -ne 1 `
        -or $metadata.SteamId -cne $SteamId `
        -or $metadata.FixedLength -ne 4326608 `
        -or $rmm.Length -ne 4326608 `
        -or $metadata.LastKnownSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'The selected RMM and save metadata are not a valid matching pair.'
    }

    $actualSha256 = (Get-FileHash -LiteralPath $RmmPath -Algorithm SHA256 -ErrorAction Stop).
        Hash.ToLowerInvariant()
    if ($actualSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'The selected RMM hash could not be calculated.'
    }
    if ($metadata.CleanExit -and $metadata.LastKnownSha256 -cne $actualSha256) {
        throw 'The selected RMM and save metadata are not a clean matching pair.'
    }

    [pscustomobject]@{
        LastKnownSha256 = $metadata.LastKnownSha256
        CleanExit = $metadata.CleanExit
        ActualSha256 = $actualSha256
    }
}

Export-ModuleMember -Function Read-StrictJson, Read-SaveMetadataState, Resolve-RmmBridgeDeploymentSaveState
