$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$safeModulePath = Join-Path $PSScriptRoot '..\SafeReleaseDirectories.psm1'
$modulePath = Join-Path $PSScriptRoot '..\ReleaseArtifactPromotion.psm1'
Import-Module $safeModulePath -Force
Import-Module $modulePath -Force
if ($null -eq (Get-Command Open-SafeReleaseRoot -ErrorAction SilentlyContinue)) {
    throw 'Importing the promotion helper removed safe-directory commands from the caller scope.'
}

$artifactNames = @(
    'binary.zip',
    'binary.zip.sha256',
    'source.zip',
    'source.zip.sha256'
)

function Assert-BytesEqual {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Expected,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $actual = [IO.File]::ReadAllBytes($Path)
    if ([Convert]::ToHexString($actual) -cne [Convert]::ToHexString($Expected)) {
        throw "Unexpected bytes at $Path"
    }
}

function Assert-NoTransactionResidue {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)

    $residue = @(Get-ChildItem `
        -LiteralPath $OutputRoot `
        -Directory `
        -Force `
        -Filter 'release-publish-transaction-*')
    if ($residue.Count -ne 0) {
        throw "Release publication left transaction residue: $($residue.FullName -join ', ')"
    }
}

function New-CaseDirectories {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $caseRoot = Join-Path $Root $Name
    $stagingRoot = Join-Path $caseRoot 'staging'
    $outputRoot = Join-Path $caseRoot 'output'
    [IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
    [IO.Directory]::CreateDirectory($outputRoot) | Out-Null
    [pscustomobject]@{
        StagingRoot = $stagingRoot
        OutputRoot = $outputRoot
    }
}

function Write-ArtifactSet {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    $result = [ordered]@{}
    for ($index = 0; $index -lt $artifactNames.Count; $index++) {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes("$Prefix-$index")
        [IO.File]::WriteAllBytes((Join-Path $Root $artifactNames[$index]), $bytes)
        $result[$artifactNames[$index]] = $bytes
    }
    return $result
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'dsr-release-artifact-promotion-' + [Guid]::NewGuid().ToString('N'))
try {
    [IO.Directory]::CreateDirectory($testRoot) | Out-Null

    $rollbackCase = New-CaseDirectories -Root $testRoot -Name 'rollback'
    $oldArtifacts = Write-ArtifactSet -Root $rollbackCase.OutputRoot -Prefix 'old'
    Write-ArtifactSet -Root $rollbackCase.StagingRoot -Prefix 'new' | Out-Null
    $promotionFailed = $false
    try {
        Publish-ReleaseArtifactSet `
            -StagingRoot $rollbackCase.StagingRoot `
            -OutputRoot $rollbackCase.OutputRoot `
            -ArtifactNames $artifactNames `
            -BeforeFileMove {
                param($Phase, $Index, $Name)
                if ($Phase -eq 'Promote' -and $Index -eq 2) {
                    throw 'controlled third promotion failure'
                }
            }
    }
    catch {
        $promotionFailed = $true
        if (-not $_.Exception.Message.Contains(
                'controlled third promotion failure',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected controlled promotion failure: $($_.Exception.Message)"
        }
    }
    if (-not $promotionFailed) {
        throw 'The third-promotion fault did not fail publication.'
    }
    foreach ($name in $artifactNames) {
        Assert-BytesEqual -Expected $oldArtifacts[$name] -Path (
            Join-Path $rollbackCase.OutputRoot $name)
    }
    Assert-NoTransactionResidue -OutputRoot $rollbackCase.OutputRoot

    $replacementCase = New-CaseDirectories -Root $testRoot -Name 'replacement'
    Write-ArtifactSet -Root $replacementCase.OutputRoot -Prefix 'old' | Out-Null
    $replacementArtifacts = Write-ArtifactSet `
        -Root $replacementCase.StagingRoot `
        -Prefix 'replacement'
    Publish-ReleaseArtifactSet `
        -StagingRoot $replacementCase.StagingRoot `
        -OutputRoot $replacementCase.OutputRoot `
        -ArtifactNames $artifactNames
    foreach ($name in $artifactNames) {
        Assert-BytesEqual -Expected $replacementArtifacts[$name] -Path (
            Join-Path $replacementCase.OutputRoot $name)
    }
    Assert-NoTransactionResidue -OutputRoot $replacementCase.OutputRoot

    $newCase = New-CaseDirectories -Root $testRoot -Name 'new'
    $newArtifacts = Write-ArtifactSet -Root $newCase.StagingRoot -Prefix 'first'
    Publish-ReleaseArtifactSet `
        -StagingRoot $newCase.StagingRoot `
        -OutputRoot $newCase.OutputRoot `
        -ArtifactNames $artifactNames
    foreach ($name in $artifactNames) {
        Assert-BytesEqual -Expected $newArtifacts[$name] -Path (
            Join-Path $newCase.OutputRoot $name)
    }
    Assert-NoTransactionResidue -OutputRoot $newCase.OutputRoot

    $partialCase = New-CaseDirectories -Root $testRoot -Name 'partial'
    $partialBytes = [byte[]](8, 6, 7, 5, 3, 0, 9)
    [IO.File]::WriteAllBytes(
        (Join-Path $partialCase.OutputRoot $artifactNames[0]),
        $partialBytes)
    Write-ArtifactSet -Root $partialCase.StagingRoot -Prefix 'new' | Out-Null
    $partialFailed = $false
    try {
        Publish-ReleaseArtifactSet `
            -StagingRoot $partialCase.StagingRoot `
            -OutputRoot $partialCase.OutputRoot `
            -ArtifactNames $artifactNames
    }
    catch {
        $partialFailed = $true
        if (-not $_.Exception.Message.Contains(
                'partial prior release artifact set',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected partial-set failure: $($_.Exception.Message)"
        }
    }
    if (-not $partialFailed) {
        throw 'A partial prior artifact set was not rejected.'
    }
    Assert-BytesEqual `
        -Expected $partialBytes `
        -Path (Join-Path $partialCase.OutputRoot $artifactNames[0])
    Assert-NoTransactionResidue -OutputRoot $partialCase.OutputRoot

    $staleCase = New-CaseDirectories -Root $testRoot -Name 'stale'
    Write-ArtifactSet -Root $staleCase.StagingRoot -Prefix 'new' | Out-Null
    $stalePath = Join-Path $staleCase.OutputRoot 'release-publish-transaction-stale'
    [IO.Directory]::CreateDirectory($stalePath) | Out-Null
    $staleFailed = $false
    try {
        Publish-ReleaseArtifactSet `
            -StagingRoot $staleCase.StagingRoot `
            -OutputRoot $staleCase.OutputRoot `
            -ArtifactNames $artifactNames
    }
    catch {
        $staleFailed = $true
        if (-not $_.Exception.Message.Contains(
                'stale release publication transaction',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected stale-transaction failure: $($_.Exception.Message)"
        }
    }
    if (-not $staleFailed) {
        throw 'A stale publication transaction was not rejected.'
    }
}
finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTempRoot = [IO.Path]::TrimEndingDirectorySeparator(
        [IO.Path]::GetFullPath([IO.Path]::GetTempPath()))
    if ($resolvedTestRoot.StartsWith(
        $resolvedTempRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase) `
        -and [IO.Path]::GetFileName($resolvedTestRoot).StartsWith(
            'dsr-release-artifact-promotion-',
            [StringComparison]::Ordinal) `
        -and (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
