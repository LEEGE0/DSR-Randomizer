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

function Assert-ArtifactSet {
    param(
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Expected,
        [Parameter(Mandatory = $true)][string]$Root
    )

    foreach ($name in $artifactNames) {
        Assert-BytesEqual -Expected $Expected[$name] -Path (Join-Path $Root $name)
    }
}

function Assert-NoTransactionResidue {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)

    $residue = @(Get-ChildItem -LiteralPath $OutputRoot -Directory -Force | Where-Object {
            $_.Name.StartsWith('release-publish-transaction-', [StringComparison]::Ordinal) `
                -or $_.Name.StartsWith('release-publish-committed-', [StringComparison]::Ordinal)
        })
    if ($residue.Count -ne 0) {
        throw "Release publication left transaction residue: $($residue.FullName -join ', ')"
    }
}

function Get-PublicationResidue {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)

    return @(Get-ChildItem -LiteralPath $OutputRoot -Directory -Force | Where-Object {
            $_.Name.StartsWith('release-publish-transaction-', [StringComparison]::Ordinal) `
                -or $_.Name.StartsWith('release-publish-committed-', [StringComparison]::Ordinal)
        })
}

function Assert-OneCommittedTransaction {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)

    $residue = @(Get-PublicationResidue -OutputRoot $OutputRoot)
    if ($residue.Count -ne 1) {
        throw 'A cleanup failure did not retain exactly one publication transaction.'
    }
    $journal = Get-Content `
        -LiteralPath (Join-Path $residue[0].FullName 'transaction-state.json') `
        -Raw | ConvertFrom-Json
    if ([string]$journal.phase -cne 'Committed') {
        throw 'A cleanup failure did not retain a clearly Committed transaction.'
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

    $bytesByName = [ordered]@{}
    $expectedArchiveHashes = [ordered]@{}
    foreach ($archiveIndex in @(0, 2)) {
        $archiveName = $artifactNames[$archiveIndex]
        $archiveBytes = [Text.UTF8Encoding]::new($false).GetBytes(
            "$Prefix-$archiveName")
        $archivePath = Join-Path $Root $archiveName
        [IO.File]::WriteAllBytes($archivePath, $archiveBytes)
        $bytesByName[$archiveName] = $archiveBytes

        $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $expectedArchiveHashes[$archiveName] = $hash
        $sidecarName = $artifactNames[$archiveIndex + 1]
        $sidecarBytes = [Text.UTF8Encoding]::new($false).GetBytes(
            "$hash  $archiveName`n")
        [IO.File]::WriteAllBytes((Join-Path $Root $sidecarName), $sidecarBytes)
        $bytesByName[$sidecarName] = $sidecarBytes
    }
    [pscustomobject]@{
        Bytes = $bytesByName
        ExpectedArchiveHashes = $expectedArchiveHashes
    }
}

function Get-ArtifactBlobHashes {
    param([Parameter(Mandatory = $true)][Collections.IDictionary]$Bytes)

    $result = [ordered]@{}
    foreach ($name in $artifactNames) {
        $result[$name] = [Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData([byte[]]$Bytes[$name])).ToLowerInvariant()
    }
    return $result
}

function Write-ArtifactBytes {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Bytes
    )

    foreach ($name in $artifactNames) {
        [IO.File]::WriteAllBytes((Join-Path $Root $name), [byte[]]$Bytes[$name])
    }
}

function New-RecoveryTransactionFixture {
    param(
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][ValidateSet('Prepared', 'Committed')][string]$Phase,
        [Parameter(Mandatory = $true)][object]$PriorArtifacts,
        [Parameter(Mandatory = $true)][object]$FinalArtifacts
    )

    $transactionPath = Join-Path $OutputRoot (
        'release-publish-transaction-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($transactionPath) | Out-Null
    for ($index = 0; $index -lt $artifactNames.Count; $index++) {
        [IO.File]::WriteAllBytes(
            (Join-Path $transactionPath "previous-$index"),
            [byte[]]$PriorArtifacts.Bytes[$artifactNames[$index]])
    }
    $state = [ordered]@{
        schemaVersion = 3
        phase = $Phase
        hadPriorSet = $true
        artifactNames = $artifactNames
        expectedArchiveHashes = $FinalArtifacts.ExpectedArchiveHashes
        expectedFinalBlobHashes = Get-ArtifactBlobHashes -Bytes $FinalArtifacts.Bytes
        backupBlobHashes = Get-ArtifactBlobHashes -Bytes $PriorArtifacts.Bytes
    }
    [IO.File]::WriteAllText(
        (Join-Path $transactionPath 'transaction-state.json'),
        ($state | ConvertTo-Json -Compress -Depth 4),
        [Text.UTF8Encoding]::new($false))
    return $transactionPath
}

function Invoke-TestPublication {
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedArchiveHashes,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )

    $arguments = @{
        StagingRoot = $Case.StagingRoot
        OutputRoot = $Case.OutputRoot
        ArtifactNames = $artifactNames
        ExpectedArchiveHashes = $ExpectedArchiveHashes
    }
    if ($null -ne $OperationHook) {
        $arguments.OperationHook = $OperationHook
    }
    Publish-ReleaseArtifactSet @arguments
}

function Assert-PublicationFailed {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$ExpectedMessage
    )

    $failed = $false
    try {
        & $Action
    }
    catch {
        $failed = $true
        if (-not $_.Exception.Message.Contains(
                $ExpectedMessage,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected publication failure: $($_.Exception.Message)"
        }
    }
    if (-not $failed) {
        throw "Publication did not reject: $ExpectedMessage"
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'dsr-release-artifact-promotion-' + [Guid]::NewGuid().ToString('N'))
try {
    [IO.Directory]::CreateDirectory($testRoot) | Out-Null

    $lockedStageCase = New-CaseDirectories -Root $testRoot -Name 'locked-stage-cleanup'
    $lockedOldArtifacts = Write-ArtifactSet `
        -Root $lockedStageCase.OutputRoot `
        -Prefix 'old'
    $lockedNewArtifacts = Write-ArtifactSet `
        -Root $lockedStageCase.StagingRoot `
        -Prefix 'new'
    $lockedStream = [IO.File]::Open(
        (Join-Path $lockedStageCase.StagingRoot $artifactNames[0]),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::None)
    try {
        Assert-PublicationFailed `
            -ExpectedMessage 'lease staged release artifact' `
            -Action {
                Invoke-TestPublication `
                    -Case $lockedStageCase `
                    -ExpectedArchiveHashes $lockedNewArtifacts.ExpectedArchiveHashes
            }
    }
    finally {
        $lockedStream.Dispose()
    }
    Assert-ArtifactSet `
        -Expected $lockedOldArtifacts.Bytes `
        -Root $lockedStageCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $lockedStageCase.OutputRoot
    Invoke-TestPublication `
        -Case $lockedStageCase `
        -ExpectedArchiveHashes $lockedNewArtifacts.ExpectedArchiveHashes
    Assert-ArtifactSet `
        -Expected $lockedNewArtifacts.Bytes `
        -Root $lockedStageCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $lockedStageCase.OutputRoot

    $journalCase = New-CaseDirectories -Root $testRoot -Name 'journal-cleanup'
    $journalOldArtifacts = Write-ArtifactSet -Root $journalCase.OutputRoot -Prefix 'old'
    $journalNewArtifacts = Write-ArtifactSet -Root $journalCase.StagingRoot -Prefix 'new'
    Assert-PublicationFailed `
        -ExpectedMessage 'controlled journal setup failure' `
        -Action {
            Invoke-TestPublication `
                -Case $journalCase `
                -ExpectedArchiveHashes $journalNewArtifacts.ExpectedArchiveHashes `
                -OperationHook {
                    param($Phase, $Index, $Name)
                    if ($Phase -eq 'BeforeInitialJournal') {
                        throw 'controlled journal setup failure'
                    }
                }
        }
    Assert-ArtifactSet -Expected $journalOldArtifacts.Bytes -Root $journalCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $journalCase.OutputRoot

    $rollbackCase = New-CaseDirectories -Root $testRoot -Name 'rollback'
    $oldArtifacts = Write-ArtifactSet -Root $rollbackCase.OutputRoot -Prefix 'old'
    $newArtifacts = Write-ArtifactSet -Root $rollbackCase.StagingRoot -Prefix 'new'
    Assert-PublicationFailed `
        -ExpectedMessage 'controlled third publication failure' `
        -Action {
            Invoke-TestPublication `
                -Case $rollbackCase `
                -ExpectedArchiveHashes $newArtifacts.ExpectedArchiveHashes `
                -OperationHook {
                    param($Phase, $Index, $Name)
                    if ($Phase -eq 'BeforePublishCopy' -and $Index -eq 2) {
                        throw 'controlled third publication failure'
                    }
                }
        }
    Assert-ArtifactSet -Expected $oldArtifacts.Bytes -Root $rollbackCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $rollbackCase.OutputRoot

    $priorMutationCase = New-CaseDirectories -Root $testRoot -Name 'prior-mutation'
    $priorMutationOld = Write-ArtifactSet -Root $priorMutationCase.OutputRoot -Prefix 'old'
    $priorMutationNew = Write-ArtifactSet -Root $priorMutationCase.StagingRoot -Prefix 'new'
    $priorMutationWasBlocked = $false
    Assert-PublicationFailed `
        -ExpectedMessage 'controlled publication failure after prior backup' `
        -Action {
            Invoke-TestPublication `
                -Case $priorMutationCase `
                -ExpectedArchiveHashes $priorMutationNew.ExpectedArchiveHashes `
                -OperationHook {
                    param($Phase, $Index, $Name)
                    if ($Phase -eq 'BeforeBackup' -and $Index -eq 0) {
                        try {
                            [IO.File]::WriteAllBytes(
                                (Join-Path $priorMutationCase.OutputRoot $artifactNames[0]),
                                [byte[]](9, 9, 9, 9))
                        }
                        catch [IO.IOException] {
                            $script:priorMutationWasBlocked = $true
                        }
                    }
                    if ($Phase -eq 'BeforePublishCopy' -and $Index -eq 2) {
                        throw 'controlled publication failure after prior backup'
                    }
                }
        }
    if (-not $priorMutationWasBlocked) {
        throw 'The prior release lease did not block a backup-boundary mutation.'
    }
    Assert-ArtifactSet -Expected $priorMutationOld.Bytes -Root $priorMutationCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $priorMutationCase.OutputRoot

    $lockedPriorCase = New-CaseDirectories -Root $testRoot -Name 'locked-prior'
    $lockedPriorOld = Write-ArtifactSet -Root $lockedPriorCase.OutputRoot -Prefix 'old'
    $lockedPriorNew = Write-ArtifactSet -Root $lockedPriorCase.StagingRoot -Prefix 'new'
    $lockedPriorStream = [IO.File]::Open(
        (Join-Path $lockedPriorCase.OutputRoot $artifactNames[0]),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::None)
    try {
        Assert-PublicationFailed `
            -ExpectedMessage 'lease prior release artifact' `
            -Action {
                Invoke-TestPublication `
                    -Case $lockedPriorCase `
                    -ExpectedArchiveHashes $lockedPriorNew.ExpectedArchiveHashes
            }
    }
    finally {
        $lockedPriorStream.Dispose()
    }
    Assert-ArtifactSet -Expected $lockedPriorOld.Bytes -Root $lockedPriorCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $lockedPriorCase.OutputRoot

    $invalidPriorCase = New-CaseDirectories -Root $testRoot -Name 'invalid-prior-sidecar'
    $invalidPriorOld = Write-ArtifactSet -Root $invalidPriorCase.OutputRoot -Prefix 'old'
    $invalidPriorNew = Write-ArtifactSet -Root $invalidPriorCase.StagingRoot -Prefix 'new'
    $invalidPriorBytes = [Text.UTF8Encoding]::new($false).GetBytes(
        "$($invalidPriorOld.ExpectedArchiveHashes[$artifactNames[0]])  wrong.zip`n")
    [IO.File]::WriteAllBytes(
        (Join-Path $invalidPriorCase.OutputRoot $artifactNames[1]),
        $invalidPriorBytes)
    $invalidPriorOld.Bytes[$artifactNames[1]] = $invalidPriorBytes
    Assert-PublicationFailed `
        -ExpectedMessage 'Prior release checksum sidecar does not exactly match' `
        -Action {
            Invoke-TestPublication `
                -Case $invalidPriorCase `
                -ExpectedArchiveHashes $invalidPriorNew.ExpectedArchiveHashes
        }
    Assert-ArtifactSet -Expected $invalidPriorOld.Bytes -Root $invalidPriorCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $invalidPriorCase.OutputRoot

    $priorHardLinkCase = New-CaseDirectories -Root $testRoot -Name 'prior-hard-link'
    $priorHardLinkOld = Write-ArtifactSet -Root $priorHardLinkCase.OutputRoot -Prefix 'old'
    $priorHardLinkNew = Write-ArtifactSet -Root $priorHardLinkCase.StagingRoot -Prefix 'new'
    $priorAliasPath = Join-Path $priorHardLinkCase.OutputRoot 'prior-alias.bin'
    New-Item `
        -ItemType HardLink `
        -Path $priorAliasPath `
        -Target (Join-Path $priorHardLinkCase.OutputRoot $artifactNames[0]) | Out-Null
    Assert-PublicationFailed `
        -ExpectedMessage 'multiple hard links' `
        -Action {
            Invoke-TestPublication `
                -Case $priorHardLinkCase `
                -ExpectedArchiveHashes $priorHardLinkNew.ExpectedArchiveHashes
        }
    Assert-ArtifactSet -Expected $priorHardLinkOld.Bytes -Root $priorHardLinkCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $priorHardLinkCase.OutputRoot

    $mismatchedNameCase = New-CaseDirectories -Root $testRoot -Name 'sidecar-name'
    $mismatchedNameOld = Write-ArtifactSet -Root $mismatchedNameCase.OutputRoot -Prefix 'old'
    $mismatchedNameNew = Write-ArtifactSet -Root $mismatchedNameCase.StagingRoot -Prefix 'new'
    $binaryHash = $mismatchedNameNew.ExpectedArchiveHashes[$artifactNames[0]]
    [IO.File]::WriteAllText(
        (Join-Path $mismatchedNameCase.StagingRoot $artifactNames[1]),
        "$binaryHash  wrong.zip`n",
        [Text.UTF8Encoding]::new($false))
    Assert-PublicationFailed `
        -ExpectedMessage 'sidecar does not exactly match' `
        -Action {
            Invoke-TestPublication `
                -Case $mismatchedNameCase `
                -ExpectedArchiveHashes $mismatchedNameNew.ExpectedArchiveHashes
        }
    Assert-ArtifactSet -Expected $mismatchedNameOld.Bytes -Root $mismatchedNameCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $mismatchedNameCase.OutputRoot

    $wrongSidecarHashCase = New-CaseDirectories -Root $testRoot -Name 'sidecar-hash'
    $wrongSidecarOld = Write-ArtifactSet -Root $wrongSidecarHashCase.OutputRoot -Prefix 'old'
    $wrongSidecarNew = Write-ArtifactSet -Root $wrongSidecarHashCase.StagingRoot -Prefix 'new'
    [IO.File]::WriteAllText(
        (Join-Path $wrongSidecarHashCase.StagingRoot $artifactNames[1]),
        "$('0' * 64)  $($artifactNames[0])`n",
        [Text.UTF8Encoding]::new($false))
    Assert-PublicationFailed `
        -ExpectedMessage 'sidecar does not exactly match' `
        -Action {
            Invoke-TestPublication `
                -Case $wrongSidecarHashCase `
                -ExpectedArchiveHashes $wrongSidecarNew.ExpectedArchiveHashes
        }
    Assert-ArtifactSet -Expected $wrongSidecarOld.Bytes -Root $wrongSidecarHashCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $wrongSidecarHashCase.OutputRoot

    $gateMismatchCase = New-CaseDirectories -Root $testRoot -Name 'gate-mismatch'
    $gateMismatchOld = Write-ArtifactSet -Root $gateMismatchCase.OutputRoot -Prefix 'old'
    $gateMismatchNew = Write-ArtifactSet -Root $gateMismatchCase.StagingRoot -Prefix 'new'
    $wrongExpectedHashes = [ordered]@{}
    foreach ($archiveName in @($artifactNames[0], $artifactNames[2])) {
        $wrongExpectedHashes[$archiveName] = $gateMismatchNew.ExpectedArchiveHashes[$archiveName]
    }
    $wrongExpectedHashes[$artifactNames[0]] = 'f' * 64
    Assert-PublicationFailed `
        -ExpectedMessage 'does not match its expected gated SHA-256' `
        -Action {
            Invoke-TestPublication `
                -Case $gateMismatchCase `
                -ExpectedArchiveHashes $wrongExpectedHashes
        }
    Assert-ArtifactSet -Expected $gateMismatchOld.Bytes -Root $gateMismatchCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $gateMismatchCase.OutputRoot

    $replacementAttemptCase = New-CaseDirectories -Root $testRoot -Name 'leased-replacement'
    Write-ArtifactSet -Root $replacementAttemptCase.OutputRoot -Prefix 'old' | Out-Null
    $replacementAttemptNew = Write-ArtifactSet `
        -Root $replacementAttemptCase.StagingRoot `
        -Prefix 'new'
    $replacementWasBlocked = $false
    Invoke-TestPublication `
        -Case $replacementAttemptCase `
        -ExpectedArchiveHashes $replacementAttemptNew.ExpectedArchiveHashes `
        -OperationHook {
            param($Phase, $Index, $Name)
            if ($Phase -eq 'BeforePublishCopy' -and $Index -eq 0) {
                try {
                    [IO.File]::WriteAllBytes(
                        (Join-Path $replacementAttemptCase.StagingRoot $artifactNames[0]),
                        [byte[]](1, 3, 3, 7))
                }
                catch [IO.IOException] {
                    $script:replacementWasBlocked = $true
                }
            }
        }
    if (-not $replacementWasBlocked) {
        throw 'The staged archive lease did not block a copy-boundary replacement attempt.'
    }
    Assert-ArtifactSet `
        -Expected $replacementAttemptNew.Bytes `
        -Root $replacementAttemptCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $replacementAttemptCase.OutputRoot

    $postWriteCase = New-CaseDirectories -Root $testRoot -Name 'post-write-tamper'
    $postWriteOld = Write-ArtifactSet -Root $postWriteCase.OutputRoot -Prefix 'old'
    $postWriteNew = Write-ArtifactSet -Root $postWriteCase.StagingRoot -Prefix 'new'
    Assert-PublicationFailed `
        -ExpectedMessage 'does not match its expected gated SHA-256' `
        -Action {
            Invoke-TestPublication `
                -Case $postWriteCase `
                -ExpectedArchiveHashes $postWriteNew.ExpectedArchiveHashes `
                -OperationHook {
                    param($Phase, $Index, $Name)
                    if ($Phase -eq 'BeforeFinalVerification') {
                        [IO.File]::WriteAllBytes(
                            (Join-Path $postWriteCase.OutputRoot $artifactNames[0]),
                            [byte[]](2, 4, 6, 8))
                    }
                }
        }
    Assert-ArtifactSet -Expected $postWriteOld.Bytes -Root $postWriteCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $postWriteCase.OutputRoot

    $replacementCase = New-CaseDirectories -Root $testRoot -Name 'replacement'
    Write-ArtifactSet -Root $replacementCase.OutputRoot -Prefix 'old' | Out-Null
    $replacementArtifacts = Write-ArtifactSet `
        -Root $replacementCase.StagingRoot `
        -Prefix 'replacement'
    Invoke-TestPublication `
        -Case $replacementCase `
        -ExpectedArchiveHashes $replacementArtifacts.ExpectedArchiveHashes
    Assert-ArtifactSet -Expected $replacementArtifacts.Bytes -Root $replacementCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $replacementCase.OutputRoot

    foreach ($lockedBackupIndex in @(0, 3)) {
        $committedCleanupCase = New-CaseDirectories `
            -Root $testRoot `
            -Name "committed-cleanup-$lockedBackupIndex"
        Write-ArtifactSet -Root $committedCleanupCase.OutputRoot -Prefix 'old' | Out-Null
        $committedCleanupNew = Write-ArtifactSet `
            -Root $committedCleanupCase.StagingRoot `
            -Prefix 'new'
        $cleanupLock = $null
        try {
            Assert-PublicationFailed `
                -ExpectedMessage 'committed' `
                -Action {
                    Invoke-TestPublication `
                        -Case $committedCleanupCase `
                        -ExpectedArchiveHashes $committedCleanupNew.ExpectedArchiveHashes `
                        -OperationHook {
                            param($Phase, $Index, $Name)
                            if ($Phase -eq 'BeforeCommittedBackupCleanup' `
                                    -and $Index -eq $lockedBackupIndex) {
                                $transactionPath = @(Get-PublicationResidue `
                                    -OutputRoot $committedCleanupCase.OutputRoot)[0].FullName
                                $script:cleanupLock = [IO.File]::Open(
                                    (Join-Path $transactionPath "previous-$Index"),
                                    [IO.FileMode]::Open,
                                    [IO.FileAccess]::Read,
                                    [IO.FileShare]::Read)
                            }
                        }
                }
            Assert-ArtifactSet `
                -Expected $committedCleanupNew.Bytes `
                -Root $committedCleanupCase.OutputRoot
            Assert-OneCommittedTransaction -OutputRoot $committedCleanupCase.OutputRoot
        }
        finally {
            if ($null -ne $cleanupLock) {
                $cleanupLock.Dispose()
            }
        }
        Invoke-TestPublication `
            -Case $committedCleanupCase `
            -ExpectedArchiveHashes $committedCleanupNew.ExpectedArchiveHashes
        Assert-ArtifactSet `
            -Expected $committedCleanupNew.Bytes `
            -Root $committedCleanupCase.OutputRoot
        Assert-NoTransactionResidue -OutputRoot $committedCleanupCase.OutputRoot
    }

    foreach ($cleanupPhase in @(
            'BeforeCommittedJournalCleanup',
            'BeforeCommittedDirectoryCleanup')) {
        $cleanupFaultCase = New-CaseDirectories `
            -Root $testRoot `
            -Name $cleanupPhase
        Write-ArtifactSet -Root $cleanupFaultCase.OutputRoot -Prefix 'old' | Out-Null
        $cleanupFaultNew = Write-ArtifactSet `
            -Root $cleanupFaultCase.StagingRoot `
            -Prefix 'new'
        Assert-PublicationFailed `
            -ExpectedMessage 'committed' `
            -Action {
                Invoke-TestPublication `
                    -Case $cleanupFaultCase `
                    -ExpectedArchiveHashes $cleanupFaultNew.ExpectedArchiveHashes `
                    -OperationHook {
                        param($Phase, $Index, $Name)
                        if ($Phase -eq $cleanupPhase) {
                            throw "controlled committed cleanup fault: $cleanupPhase"
                        }
                    }
            }
        Assert-ArtifactSet `
            -Expected $cleanupFaultNew.Bytes `
            -Root $cleanupFaultCase.OutputRoot
        Assert-OneCommittedTransaction -OutputRoot $cleanupFaultCase.OutputRoot
    }

    $newCase = New-CaseDirectories -Root $testRoot -Name 'new'
    $newArtifacts = Write-ArtifactSet -Root $newCase.StagingRoot -Prefix 'first'
    Invoke-TestPublication `
        -Case $newCase `
        -ExpectedArchiveHashes $newArtifacts.ExpectedArchiveHashes
    Assert-ArtifactSet -Expected $newArtifacts.Bytes -Root $newCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $newCase.OutputRoot

    $partialCase = New-CaseDirectories -Root $testRoot -Name 'partial'
    $partialBytes = [byte[]](8, 6, 7, 5, 3, 0, 9)
    [IO.File]::WriteAllBytes(
        (Join-Path $partialCase.OutputRoot $artifactNames[0]),
        $partialBytes)
    $partialNew = Write-ArtifactSet -Root $partialCase.StagingRoot -Prefix 'new'
    Assert-PublicationFailed `
        -ExpectedMessage 'partial prior release artifact set' `
        -Action {
            Invoke-TestPublication `
                -Case $partialCase `
                -ExpectedArchiveHashes $partialNew.ExpectedArchiveHashes
        }
    Assert-BytesEqual `
        -Expected $partialBytes `
        -Path (Join-Path $partialCase.OutputRoot $artifactNames[0])
    Assert-NoTransactionResidue -OutputRoot $partialCase.OutputRoot

    $preparedRecoveryCase = New-CaseDirectories -Root $testRoot -Name 'prepared-recovery'
    $preparedRecoveryOld = Write-ArtifactSet `
        -Root $preparedRecoveryCase.OutputRoot `
        -Prefix 'old'
    $preparedRecoveryNew = Write-ArtifactSet `
        -Root $preparedRecoveryCase.StagingRoot `
        -Prefix 'new'
    New-RecoveryTransactionFixture `
        -OutputRoot $preparedRecoveryCase.OutputRoot `
        -Phase 'Prepared' `
        -PriorArtifacts $preparedRecoveryOld `
        -FinalArtifacts $preparedRecoveryNew | Out-Null
    Write-ArtifactBytes `
        -Root $preparedRecoveryCase.OutputRoot `
        -Bytes $preparedRecoveryNew.Bytes
    Assert-PublicationFailed `
        -ExpectedMessage 'controlled stop after recovery' `
        -Action {
            Invoke-TestPublication `
                -Case $preparedRecoveryCase `
                -ExpectedArchiveHashes $preparedRecoveryNew.ExpectedArchiveHashes `
                -OperationHook {
                    param($Phase, $Index, $Name)
                    if ($Phase -eq 'AfterRecovery') {
                        throw 'controlled stop after recovery'
                    }
                }
        }
    Assert-ArtifactSet `
        -Expected $preparedRecoveryOld.Bytes `
        -Root $preparedRecoveryCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $preparedRecoveryCase.OutputRoot

    $committedRecoveryCase = New-CaseDirectories -Root $testRoot -Name 'committed-recovery'
    $committedRecoveryOldRoot = Join-Path $testRoot 'committed-recovery-old'
    [IO.Directory]::CreateDirectory($committedRecoveryOldRoot) | Out-Null
    $committedRecoveryOld = Write-ArtifactSet `
        -Root $committedRecoveryOldRoot `
        -Prefix 'old'
    $committedRecoveryNew = Write-ArtifactSet `
        -Root $committedRecoveryCase.StagingRoot `
        -Prefix 'new'
    Write-ArtifactBytes `
        -Root $committedRecoveryCase.OutputRoot `
        -Bytes $committedRecoveryNew.Bytes
    New-RecoveryTransactionFixture `
        -OutputRoot $committedRecoveryCase.OutputRoot `
        -Phase 'Committed' `
        -PriorArtifacts $committedRecoveryOld `
        -FinalArtifacts $committedRecoveryNew | Out-Null
    Assert-PublicationFailed `
        -ExpectedMessage 'controlled stop after recovery' `
        -Action {
            Invoke-TestPublication `
                -Case $committedRecoveryCase `
                -ExpectedArchiveHashes $committedRecoveryNew.ExpectedArchiveHashes `
                -OperationHook {
                    param($Phase, $Index, $Name)
                    if ($Phase -eq 'AfterRecovery') {
                        throw 'controlled stop after recovery'
                    }
                }
        }
    Assert-ArtifactSet `
        -Expected $committedRecoveryNew.Bytes `
        -Root $committedRecoveryCase.OutputRoot
    Assert-NoTransactionResidue -OutputRoot $committedRecoveryCase.OutputRoot

    $staleCase = New-CaseDirectories -Root $testRoot -Name 'stale'
    $staleNew = Write-ArtifactSet -Root $staleCase.StagingRoot -Prefix 'new'
    $stalePath = Join-Path $staleCase.OutputRoot 'release-publish-transaction-stale'
    [IO.Directory]::CreateDirectory($stalePath) | Out-Null
    Assert-PublicationFailed `
        -ExpectedMessage 'stale release publication transaction' `
        -Action {
            Invoke-TestPublication `
                -Case $staleCase `
                -ExpectedArchiveHashes $staleNew.ExpectedArchiveHashes
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
