$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$modulePath = Join-Path $PSScriptRoot '..\ReleasePrivacy.psm1'
Import-Module $modulePath -Force

function Assert-ThrowsPrivacy {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][object[]]$Sentinels
    )

    $files = [Collections.Generic.SortedDictionary[string, byte[]]]::new(
        [StringComparer]::Ordinal)
    $files.Add('fixture.bin', $Bytes)
    try {
        Assert-ReleasePrivacyBytes -Files $files -Sentinels $Sentinels
    }
    catch {
        if (-not $_.Exception.Message.Contains(
                'privacy sentinel',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected privacy failure: $($_.Exception.Message)"
        }
        return
    }
    throw 'The privacy scanner accepted a known fixture leak.'
}

$syntheticUser = 'Fixture' + 'Leak'
$syntheticAccount = '314' + '159' + '265'
$sentinels = @(New-ReleasePrivacySentinelSet `
    -LocalUserName $syntheticUser `
    -ShortAccountId $syntheticAccount)

$windowsPath = "C:\Users\$syntheticUser"
$forms = @(
    $windowsPath,
    $windowsPath.Replace('\', '\\'),
    "C:/Users/$syntheticUser",
    "file:///C:/Users/$syntheticUser",
    $syntheticAccount
)
$encodings = @(
    [Text.UTF8Encoding]::new($false),
    [Text.UnicodeEncoding]::new($false, $false),
    [Text.UnicodeEncoding]::new($true, $false)
)
foreach ($form in $forms) {
    foreach ($encoding in $encodings) {
        Assert-ThrowsPrivacy `
            -Bytes $encoding.GetBytes("prefix-$form-suffix") `
            -Sentinels $sentinels
    }
}

$cleanFiles = [Collections.Generic.SortedDictionary[string, byte[]]]::new(
    [StringComparer]::Ordinal)
$cleanFiles.Add(
    'clean.txt',
    [Text.UTF8Encoding]::new($false).GetBytes('neutral release source'))
$cleanFiles.Add('empty.txt', [byte[]]::new(0))
Assert-ReleasePrivacyBytes -Files $cleanFiles -Sentinels $sentinels
