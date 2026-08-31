$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$modulePath = Join-Path $PSScriptRoot '..\ReleaseRedistributable.psm1'
Import-Module $modulePath -Force

$version = '0.1.0-alpha.2'
$outerName = "DSR-for-MOD-v$version-redistributable.zip"
$sourceName = "DSR-for-MOD-v$version-source.zip"
$binaryName = "DSR-for-MOD-v$version-win-x64.zip"
$outerEntries = @($sourceName, $binaryName, 'SHA256SUMS.txt')

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

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

function Assert-Failed {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Message
    )
    try {
        & $Action
    }
    catch {
        if (-not $_.Exception.Message.Contains($Message, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected failure: $($_.Exception.Message)"
        }
        return
    }
    throw "Operation did not fail with: $Message"
}

function New-Case {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $caseRoot = Join-Path $Root $Name
    $innerRoot = Join-Path $caseRoot 'inner'
    $stagingRoot = Join-Path $caseRoot 'staging'
    $outputRoot = Join-Path $caseRoot 'output'
    [IO.Directory]::CreateDirectory($innerRoot) | Out-Null
    [IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
    [IO.Directory]::CreateDirectory($outputRoot) | Out-Null
    [pscustomobject]@{
        InnerRoot = $innerRoot
        StagingRoot = $stagingRoot
        OutputRoot = $outputRoot
    }
}

function New-OuterFixture {
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][string]$Prefix,
        [Parameter(Mandatory = $false)][string]$StagingRoot = $Case.StagingRoot
    )
    $fixtureRoot = Join-Path $Case.InnerRoot $Prefix
    [IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    $sourcePath = Join-Path $fixtureRoot $sourceName
    $binaryPath = Join-Path $fixtureRoot $binaryName
    [IO.File]::WriteAllBytes(
        $sourcePath,
        [Text.UTF8Encoding]::new($false).GetBytes("$Prefix-source"))
    [IO.File]::WriteAllBytes(
        $binaryPath,
        [Text.UTF8Encoding]::new($false).GetBytes("$Prefix-binary"))
    [IO.Directory]::CreateDirectory($StagingRoot) | Out-Null
    $outerPath = Join-Path $StagingRoot $outerName
    New-ReleaseRedistributableArchive `
        -Version $version `
        -SourceArchivePath $sourcePath `
        -BinaryArchivePath $binaryPath `
        -OutputPath $outerPath
    Assert-ReleaseRedistributableArchive `
        -ArchivePath $outerPath `
        -Version $version | Out-Null
    [pscustomobject]@{
        Path = $outerPath
        Bytes = [IO.File]::ReadAllBytes($outerPath)
        Hash = Get-Sha256 -Path $outerPath
        SourceHash = Get-Sha256 -Path $sourcePath
        BinaryHash = Get-Sha256 -Path $binaryPath
    }
}

function Publish-Fixture {
    param(
        [Parameter(Mandatory = $true)][object]$Case,
        [Parameter(Mandatory = $true)][object]$Fixture,
        [Parameter(Mandatory = $false)][scriptblock]$OperationHook
    )
    $arguments = @{
        StagedArchivePath = $Fixture.Path
        OutputRoot = $Case.OutputRoot
        FileName = $outerName
        ExpectedSha256 = $Fixture.Hash
        Version = $version
    }
    if ($null -ne $OperationHook) {
        $arguments.OperationHook = $OperationHook
    }
    Publish-ReleaseRedistributableArchive @arguments
}

function Assert-NoTransactionState {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)
    $unexpected = @(Get-ChildItem -LiteralPath $OutputRoot -Force | Where-Object {
            $_.PSIsContainer `
                -or $_.Name.Contains('transaction', [StringComparison]::OrdinalIgnoreCase) `
                -or $_.Name.Contains('journal', [StringComparison]::OrdinalIgnoreCase)
        })
    if ($unexpected.Count -ne 0) {
        throw "Single-file publication left transaction state: $($unexpected.FullName -join ', ')"
    }
}

function Assert-NoPublicationResidue {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)
    $unexpected = @(Get-ChildItem -LiteralPath $OutputRoot -File -Force | Where-Object {
            $_.Name.Contains('.pending-', [StringComparison]::Ordinal) `
                -or $_.Name.Contains('.failed-', [StringComparison]::Ordinal) `
                -or $_.Name.Contains('.rollback-', [StringComparison]::Ordinal)
        })
    if ($unexpected.Count -ne 0) {
        throw "Single-file publication left unsafe file residue: $($unexpected.FullName -join ', ')"
    }
}

function Wait-ForSignal {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process
    )
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($Process.HasExited) {
            throw "Synchronized publisher exited early: $($Process.StandardError.ReadToEnd())"
        }
        if ($watch.Elapsed -gt [TimeSpan]::FromSeconds(15)) {
            throw 'Timed out waiting for synchronized publisher.'
        }
        [Threading.Thread]::Sleep(20)
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'dsr-release-redistributable-' + [Guid]::NewGuid().ToString('N'))
try {
    [IO.Directory]::CreateDirectory($testRoot) | Out-Null

    $deterministicCase = New-Case -Root $testRoot -Name 'deterministic'
    $first = New-OuterFixture -Case $deterministicCase -Prefix 'same'
    $secondRoot = Join-Path $deterministicCase.StagingRoot 'second'
    $second = New-OuterFixture `
        -Case $deterministicCase `
        -Prefix 'same' `
        -StagingRoot $secondRoot
    if ($first.Hash -cne $second.Hash) {
        throw 'Equivalent redistributable inputs did not produce deterministic bytes.'
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($first.Path)
    try {
        $names = @($archive.Entries | ForEach-Object FullName)
        if (@(Compare-Object $outerEntries $names -SyncWindow 0).Count -ne 0) {
            throw 'Redistributable outer entry order is not exact.'
        }
        foreach ($entry in $archive.Entries) {
            if ($entry.LastWriteTime.DateTime -ne [datetime]'1980-01-01') {
                throw "Redistributable timestamp is not fixed: $($entry.FullName)"
            }
        }
        $sumsEntry = @($archive.Entries | Where-Object FullName -CEQ 'SHA256SUMS.txt')[0]
        $reader = [IO.StreamReader]::new(
            $sumsEntry.Open(),
            [Text.UTF8Encoding]::new($false, $true))
        try {
            $sums = $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
        $expectedSums = "$($first.SourceHash)  $sourceName`n$($first.BinaryHash)  $binaryName`n"
        if ($sums -cne $expectedSums) {
            throw 'SHA256SUMS.txt is not the exact strict two-line manifest.'
        }
    }
    finally {
        $archive.Dispose()
    }

    $newCase = New-Case -Root $testRoot -Name 'new'
    $newFixture = New-OuterFixture -Case $newCase -Prefix 'new'
    Publish-Fixture -Case $newCase -Fixture $newFixture
    Assert-BytesEqual `
        -Expected $newFixture.Bytes `
        -Path (Join-Path $newCase.OutputRoot $outerName)
    Assert-NoTransactionState -OutputRoot $newCase.OutputRoot

    $replaceCase = New-Case -Root $testRoot -Name 'replace'
    $oldFixture = New-OuterFixture -Case $replaceCase -Prefix 'old'
    Publish-Fixture -Case $replaceCase -Fixture $oldFixture
    $newStaging = Join-Path $replaceCase.StagingRoot 'replacement'
    $replacementFixture = New-OuterFixture `
        -Case $replaceCase `
        -Prefix 'replacement' `
        -StagingRoot $newStaging
    Publish-Fixture -Case $replaceCase -Fixture $replacementFixture
    Assert-BytesEqual `
        -Expected $replacementFixture.Bytes `
        -Path (Join-Path $replaceCase.OutputRoot $outerName)
    Assert-NoTransactionState -OutputRoot $replaceCase.OutputRoot

    $preFailureCase = New-Case -Root $testRoot -Name 'pre-replace-failure'
    $preOld = New-OuterFixture -Case $preFailureCase -Prefix 'old'
    Publish-Fixture -Case $preFailureCase -Fixture $preOld
    $preNewRoot = Join-Path $preFailureCase.StagingRoot 'new'
    $preNew = New-OuterFixture -Case $preFailureCase -Prefix 'new' -StagingRoot $preNewRoot
    Assert-Failed `
        -Message 'controlled pre-replace failure' `
        -Action {
            Publish-Fixture `
                -Case $preFailureCase `
                -Fixture $preNew `
                -OperationHook {
                    param($Phase, $Path)
                    if ($Phase -eq 'BeforeAtomicReplace') {
                        throw 'controlled pre-replace failure'
                    }
                }
        }
    Assert-BytesEqual `
        -Expected $preOld.Bytes `
        -Path (Join-Path $preFailureCase.OutputRoot $outerName)
    Assert-NoTransactionState -OutputRoot $preFailureCase.OutputRoot

    $postFailureCase = New-Case -Root $testRoot -Name 'post-replace-failure'
    $postOld = New-OuterFixture -Case $postFailureCase -Prefix 'old'
    Publish-Fixture -Case $postFailureCase -Fixture $postOld
    $postNewRoot = Join-Path $postFailureCase.StagingRoot 'new'
    $postNew = New-OuterFixture -Case $postFailureCase -Prefix 'new' -StagingRoot $postNewRoot
    Assert-Failed `
        -Message 'controlled post-replace verification failure' `
        -Action {
            Publish-Fixture `
                -Case $postFailureCase `
                -Fixture $postNew `
                -OperationHook {
                    param($Phase, $Path)
                    if ($Phase -eq 'BeforeFinalVerification') {
                        throw 'controlled post-replace verification failure'
                    }
                }
        }
    Assert-BytesEqual `
        -Expected $postOld.Bytes `
        -Path (Join-Path $postFailureCase.OutputRoot $outerName)
    Assert-NoTransactionState -OutputRoot $postFailureCase.OutputRoot

    # Catches a publisher that validates one pending object, then renames a
    # different object selected through the same mutable pathname.
    $pendingSwapCase = New-Case -Root $testRoot -Name 'pending-path-swap'
    $pendingExpected = New-OuterFixture -Case $pendingSwapCase -Prefix 'expected'
    $pendingThirdRoot = Join-Path $pendingSwapCase.StagingRoot 'third'
    $pendingThird = New-OuterFixture `
        -Case $pendingSwapCase `
        -Prefix 'third' `
        -StagingRoot $pendingThirdRoot
    $script:pendingSwapBlocked = $false
    Publish-Fixture `
        -Case $pendingSwapCase `
        -Fixture $pendingExpected `
        -OperationHook {
            param($Phase, $Path)
            if ($Phase -eq 'BeforeAtomicReplace') {
                $pending = @(Get-ChildItem `
                        -LiteralPath $pendingSwapCase.OutputRoot `
                        -File `
                        -Filter '.*.pending-*')
                if ($pending.Count -ne 1) {
                    throw "Expected one pending publication, found $($pending.Count)."
                }
                try {
                    [IO.File]::Copy($pendingThird.Path, $pending[0].FullName, $true)
                }
                catch [IO.IOException] {
                    $script:pendingSwapBlocked = $true
                }
            }
        }
    if (-not $script:pendingSwapBlocked) {
        throw 'Pending-path replacement was not blocked by the owned handle.'
    }
    Assert-BytesEqual `
        -Expected $pendingExpected.Bytes `
        -Path (Join-Path $pendingSwapCase.OutputRoot $outerName)
    Assert-NoPublicationResidue -OutputRoot $pendingSwapCase.OutputRoot

    # Catches rollback that trusts a released .previous pathname instead of
    # the exact prior bytes held by the publisher.
    $backupSwapCase = New-Case -Root $testRoot -Name 'backup-path-swap'
    $backupOld = New-OuterFixture -Case $backupSwapCase -Prefix 'old'
    Publish-Fixture -Case $backupSwapCase -Fixture $backupOld
    $backupNewRoot = Join-Path $backupSwapCase.StagingRoot 'new'
    $backupNew = New-OuterFixture `
        -Case $backupSwapCase `
        -Prefix 'new' `
        -StagingRoot $backupNewRoot
    $backupThirdRoot = Join-Path $backupSwapCase.StagingRoot 'third'
    $backupThird = New-OuterFixture `
        -Case $backupSwapCase `
        -Prefix 'third' `
        -StagingRoot $backupThirdRoot
    $script:backupSwapBlocked = $false
    $script:rollbackBackupSwapBlocked = $false
    $script:rollbackCandidateSwapBlocked = $false
    Assert-Failed `
        -Message 'controlled rollback verification failure' `
        -Action {
            Publish-Fixture `
                -Case $backupSwapCase `
                -Fixture $backupNew `
                -OperationHook {
                    param($Phase, $Path)
                    if ($Phase -eq 'BeforeFinalVerification') {
                        $backup = Join-Path `
                            $backupSwapCase.OutputRoot `
                            "$outerName.previous"
                        try {
                            [IO.File]::Copy($backupThird.Path, $backup, $true)
                        }
                        catch [IO.IOException] {
                            $script:backupSwapBlocked = $true
                        }
                        throw 'controlled rollback verification failure'
                    }
                    if ($Phase -eq 'BeforeRollback') {
                        try {
                            [IO.File]::Copy($backupThird.Path, $Path, $true)
                        }
                        catch [IO.IOException] {
                            $script:rollbackBackupSwapBlocked = $true
                        }
                        $rollback = @(Get-ChildItem `
                                -LiteralPath $backupSwapCase.OutputRoot `
                                -File `
                                -Filter '.*.rollback-*')
                        if ($rollback.Count -ne 1) {
                            throw "Expected one rollback candidate, found $($rollback.Count)."
                        }
                        try {
                            [IO.File]::Copy(
                                $backupThird.Path,
                                $rollback[0].FullName,
                                $true)
                        }
                        catch [IO.IOException] {
                            $script:rollbackCandidateSwapBlocked = $true
                        }
                    }
                }
        }
    if (-not $script:backupSwapBlocked) {
        throw 'Rollback-backup replacement was not blocked by its owned handle.'
    }
    if (-not $script:rollbackBackupSwapBlocked `
            -or -not $script:rollbackCandidateSwapBlocked) {
        throw 'Rollback did not retain stable handles for backup and restore candidate.'
    }
    Assert-BytesEqual `
        -Expected $backupOld.Bytes `
        -Path (Join-Path $backupSwapCase.OutputRoot $outerName)
    Assert-NoPublicationResidue -OutputRoot $backupSwapCase.OutputRoot
    if (Test-Path -LiteralPath (Join-Path $backupSwapCase.OutputRoot "$outerName.previous")) {
        throw 'Successful rollback left a whole-file backup behind.'
    }
    Publish-Fixture -Case $backupSwapCase -Fixture $backupNew
    Assert-BytesEqual `
        -Expected $backupNew.Bytes `
        -Path (Join-Path $backupSwapCase.OutputRoot $outerName)

    # Catches releasing the final handle before whole-backup cleanup.
    $finalSwapCase = New-Case -Root $testRoot -Name 'final-cleanup-path-swap'
    $finalOld = New-OuterFixture -Case $finalSwapCase -Prefix 'old'
    Publish-Fixture -Case $finalSwapCase -Fixture $finalOld
    $finalNewRoot = Join-Path $finalSwapCase.StagingRoot 'new'
    $finalNew = New-OuterFixture `
        -Case $finalSwapCase `
        -Prefix 'new' `
        -StagingRoot $finalNewRoot
    $finalThirdRoot = Join-Path $finalSwapCase.StagingRoot 'third'
    $finalThird = New-OuterFixture `
        -Case $finalSwapCase `
        -Prefix 'third' `
        -StagingRoot $finalThirdRoot
    $script:finalSwapBlocked = $false
    Publish-Fixture `
        -Case $finalSwapCase `
        -Fixture $finalNew `
        -OperationHook {
            param($Phase, $Path)
            if ($Phase -eq 'BeforeBackupCleanup') {
                $canonical = Join-Path $finalSwapCase.OutputRoot $outerName
                try {
                    [IO.File]::Copy($finalThird.Path, $canonical, $true)
                }
                catch [IO.IOException] {
                    $script:finalSwapBlocked = $true
                }
            }
        }
    if (-not $script:finalSwapBlocked) {
        throw 'Canonical replacement was not blocked during backup cleanup.'
    }
    Assert-BytesEqual `
        -Expected $finalNew.Bytes `
        -Path (Join-Path $finalSwapCase.OutputRoot $outerName)

    $lockedCase = New-Case -Root $testRoot -Name 'locked-canonical'
    $lockedOld = New-OuterFixture -Case $lockedCase -Prefix 'old'
    Publish-Fixture -Case $lockedCase -Fixture $lockedOld
    $lockedNewRoot = Join-Path $lockedCase.StagingRoot 'new'
    $lockedNew = New-OuterFixture `
        -Case $lockedCase `
        -Prefix 'new' `
        -StagingRoot $lockedNewRoot
    $lockedCanonical = Join-Path $lockedCase.OutputRoot $outerName
    $canonicalLock = [IO.FileStream]::new(
        $lockedCanonical,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        Assert-Failed `
            -Message 'publication' `
            -Action {
                Publish-Fixture -Case $lockedCase -Fixture $lockedNew
            }
        Assert-BytesEqual -Expected $lockedOld.Bytes -Path $lockedCanonical
        Assert-NoPublicationResidue -OutputRoot $lockedCase.OutputRoot
    }
    finally {
        $canonicalLock.Dispose()
    }
    Publish-Fixture -Case $lockedCase -Fixture $lockedNew
    Assert-BytesEqual -Expected $lockedNew.Bytes -Path $lockedCanonical

    $firstPostFailureCase = New-Case -Root $testRoot -Name 'first-post-rename-failure'
    $firstPost = New-OuterFixture -Case $firstPostFailureCase -Prefix 'new'
    Assert-Failed `
        -Message 'controlled first post-rename failure' `
        -Action {
            Publish-Fixture `
                -Case $firstPostFailureCase `
                -Fixture $firstPost `
                -OperationHook {
                    param($Phase, $Path)
                    if ($Phase -eq 'BeforeFinalVerification') {
                        throw 'controlled first post-rename failure'
                    }
                }
        }
    if (Test-Path -LiteralPath (Join-Path $firstPostFailureCase.OutputRoot $outerName)) {
        throw 'Failed first publication left a canonical archive.'
    }
    Assert-NoPublicationResidue -OutputRoot $firstPostFailureCase.OutputRoot
    Publish-Fixture -Case $firstPostFailureCase -Fixture $firstPost
    Assert-BytesEqual `
        -Expected $firstPost.Bytes `
        -Path (Join-Path $firstPostFailureCase.OutputRoot $outerName)

    $cleanupCase = New-Case -Root $testRoot -Name 'backup-cleanup-failure'
    $cleanupOld = New-OuterFixture -Case $cleanupCase -Prefix 'old'
    Publish-Fixture -Case $cleanupCase -Fixture $cleanupOld
    $cleanupNewRoot = Join-Path $cleanupCase.StagingRoot 'new'
    $cleanupNew = New-OuterFixture `
        -Case $cleanupCase `
        -Prefix 'new' `
        -StagingRoot $cleanupNewRoot
    $script:lockedBackup = $null
    Publish-Fixture `
        -Case $cleanupCase `
        -Fixture $cleanupNew `
        -OperationHook {
            param($Phase, $Path)
            if ($Phase -eq 'BeforeBackupCleanup') {
                $script:lockedBackup = [IO.FileStream]::new(
                    $Path,
                    [IO.FileMode]::Open,
                    [IO.FileAccess]::Read,
                    [IO.FileShare]::Read)
            }
        }
    try {
        Assert-BytesEqual `
            -Expected $cleanupNew.Bytes `
            -Path (Join-Path $cleanupCase.OutputRoot $outerName)
        Assert-BytesEqual `
            -Expected $cleanupOld.Bytes `
            -Path (Join-Path $cleanupCase.OutputRoot "$outerName.previous")
    }
    finally {
        if ($null -ne $script:lockedBackup) {
            $script:lockedBackup.Dispose()
            $script:lockedBackup = $null
        }
    }
    Publish-Fixture -Case $cleanupCase -Fixture $cleanupNew
    Assert-BytesEqual `
        -Expected $cleanupNew.Bytes `
        -Path (Join-Path $cleanupCase.OutputRoot $outerName)
    if (Test-Path -LiteralPath (Join-Path $cleanupCase.OutputRoot "$outerName.previous")) {
        throw 'A retry did not clean the unlocked whole-archive backup.'
    }
    Assert-NoTransactionState -OutputRoot $cleanupCase.OutputRoot

    $cleanupFaultCase = New-Case -Root $testRoot -Name 'backup-cleanup-fault'
    $cleanupFaultOld = New-OuterFixture -Case $cleanupFaultCase -Prefix 'old'
    Publish-Fixture -Case $cleanupFaultCase -Fixture $cleanupFaultOld
    $cleanupFaultNewRoot = Join-Path $cleanupFaultCase.StagingRoot 'new'
    $cleanupFaultNew = New-OuterFixture `
        -Case $cleanupFaultCase `
        -Prefix 'new' `
        -StagingRoot $cleanupFaultNewRoot
    Publish-Fixture `
        -Case $cleanupFaultCase `
        -Fixture $cleanupFaultNew `
        -OperationHook {
            param($Phase, $Path)
            if ($Phase -eq 'BeforeBackupCleanup') {
                throw 'controlled backup cleanup failure'
            }
        }
    Assert-BytesEqual `
        -Expected $cleanupFaultNew.Bytes `
        -Path (Join-Path $cleanupFaultCase.OutputRoot $outerName)
    Assert-BytesEqual `
        -Expected $cleanupFaultOld.Bytes `
        -Path (Join-Path $cleanupFaultCase.OutputRoot "$outerName.previous")
    Assert-NoTransactionState -OutputRoot $cleanupFaultCase.OutputRoot

    $workerPath = Join-Path $testRoot 'publisher-worker.ps1'
    $worker = @'
param($ModulePath, $StagedPath, $OutputRoot, $FileName, $Hash, $Version, $Ready, $Release)
$ErrorActionPreference = 'Stop'
Import-Module $ModulePath -Force
try {
    Publish-ReleaseRedistributableArchive `
        -StagedArchivePath $StagedPath `
        -OutputRoot $OutputRoot `
        -FileName $FileName `
        -ExpectedSha256 $Hash `
        -Version $Version `
        -OperationHook {
            param($Phase, $Path)
            if ($Phase -eq 'AfterPublicationLockAcquired') {
                [IO.File]::WriteAllText($Ready, 'ready', [Text.UTF8Encoding]::new($false))
                while (-not (Test-Path -LiteralPath $Release -PathType Leaf)) {
                    [Threading.Thread]::Sleep(20)
                }
            }
        }
    exit 0
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
'@
    [IO.File]::WriteAllText($workerPath, $worker, [Text.UTF8Encoding]::new($false))
    $concurrentCase = New-Case -Root $testRoot -Name 'concurrent'
    $fixtureA = New-OuterFixture -Case $concurrentCase -Prefix 'a'
    $bStaging = Join-Path $concurrentCase.StagingRoot 'b'
    $fixtureB = New-OuterFixture -Case $concurrentCase -Prefix 'b' -StagingRoot $bStaging
    $ready = Join-Path $testRoot 'publisher.ready'
    $release = Join-Path $testRoot 'publisher.release'
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PSHOME 'pwsh.exe'
    $start.UseShellExecute = $false
    $start.RedirectStandardError = $true
    foreach ($argument in @(
            '-NoProfile',
            '-File',
            $workerPath,
            $modulePath,
            $fixtureA.Path,
            $concurrentCase.OutputRoot,
            $outerName,
            $fixtureA.Hash,
            $version,
            $ready,
            $release)) {
        $start.ArgumentList.Add($argument)
    }
    $publisherA = [Diagnostics.Process]::Start($start)
    try {
        Wait-ForSignal -Path $ready -Process $publisherA
        Assert-Failed `
            -Message 'PUBLICATION_IN_PROGRESS' `
            -Action {
                Publish-Fixture -Case $concurrentCase -Fixture $fixtureB
            }
        if (Test-Path -LiteralPath (Join-Path $concurrentCase.OutputRoot $outerName)) {
            throw 'Concurrent loser changed output while the winner held the lock.'
        }
        Assert-NoTransactionState -OutputRoot $concurrentCase.OutputRoot
        [IO.File]::WriteAllText($release, 'release', [Text.UTF8Encoding]::new($false))
        if (-not $publisherA.WaitForExit(15000)) {
            $publisherA.Kill($true)
            throw 'Synchronized publisher did not exit.'
        }
        if ($publisherA.ExitCode -ne 0) {
            throw "Synchronized publisher failed: $($publisherA.StandardError.ReadToEnd())"
        }
    }
    finally {
        if (-not $publisherA.HasExited) {
            $publisherA.Kill($true)
            $publisherA.WaitForExit()
        }
        $publisherA.Dispose()
    }
    Assert-BytesEqual `
        -Expected $fixtureA.Bytes `
        -Path (Join-Path $concurrentCase.OutputRoot $outerName)
    Publish-Fixture -Case $concurrentCase -Fixture $fixtureB
    Assert-BytesEqual `
        -Expected $fixtureB.Bytes `
        -Path (Join-Path $concurrentCase.OutputRoot $outerName)
    Assert-NoTransactionState -OutputRoot $concurrentCase.OutputRoot

    $legacyCase = New-Case -Root $testRoot -Name 'legacy-cleanup'
    $legacyNames = @(
        "DSR-for-MOD-v$version-win-x64.zip",
        "DSR-for-MOD-v$version-win-x64.zip.sha256",
        "DSR-for-MOD-v$version-source.zip",
        "DSR-for-MOD-v$version-source.zip.sha256"
    )
    foreach ($name in $legacyNames) {
        [IO.File]::WriteAllText(
            (Join-Path $legacyCase.OutputRoot $name),
            'legacy',
            [Text.UTF8Encoding]::new($false))
    }
    $authoritativeFixture = New-OuterFixture -Case $legacyCase -Prefix 'authoritative'
    Publish-Fixture -Case $legacyCase -Fixture $authoritativeFixture
    Assert-Failed `
        -Message 'Names' `
        -Action {
            Remove-LegacyReleaseArtifacts `
                -OutputRoot $legacyCase.OutputRoot `
                -Names @($outerName)
        }
    Assert-BytesEqual `
        -Expected $authoritativeFixture.Bytes `
        -Path (Join-Path $legacyCase.OutputRoot $outerName)
    Assert-Failed `
        -Message 'Version' `
        -Action {
            Remove-LegacyReleaseArtifacts `
                -OutputRoot $legacyCase.OutputRoot `
                -Version '../unsafe'
        }
    Remove-LegacyReleaseArtifacts `
        -OutputRoot $legacyCase.OutputRoot `
        -Version $version
    foreach ($name in $legacyNames) {
        if (Test-Path -LiteralPath (Join-Path $legacyCase.OutputRoot $name)) {
            throw "Legacy release artifact was not removed: $name"
        }
    }
    Assert-BytesEqual `
        -Expected $authoritativeFixture.Bytes `
        -Path (Join-Path $legacyCase.OutputRoot $outerName)

    $hardLinkCase = New-Case -Root $testRoot -Name 'legacy-hardlink'
    $hardLinkedLegacy = Join-Path $hardLinkCase.OutputRoot $legacyNames[0]
    [IO.File]::WriteAllText(
        $hardLinkedLegacy,
        'hard-linked legacy',
        [Text.UTF8Encoding]::new($false))
    $hardLinkAlias = Join-Path $hardLinkCase.OutputRoot 'preserved-hardlink.bin'
    New-Item `
        -ItemType HardLink `
        -Path $hardLinkAlias `
        -Target $hardLinkedLegacy | Out-Null
    foreach ($name in $legacyNames[1..3]) {
        [IO.File]::WriteAllText(
            (Join-Path $hardLinkCase.OutputRoot $name),
            'regular legacy',
            [Text.UTF8Encoding]::new($false))
    }
    Remove-LegacyReleaseArtifacts `
        -OutputRoot $hardLinkCase.OutputRoot `
        -Version $version
    if (-not (Test-Path -LiteralPath $hardLinkedLegacy -PathType Leaf) `
            -or -not (Test-Path -LiteralPath $hardLinkAlias -PathType Leaf)) {
        throw 'Unsafe hard-linked legacy artifact was not preserved.'
    }
    foreach ($name in $legacyNames[1..3]) {
        if (Test-Path -LiteralPath (Join-Path $hardLinkCase.OutputRoot $name)) {
            throw "Safe legacy artifact was not removed beside hard link: $name"
        }
    }
}
finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $temporaryRoot = [IO.Path]::TrimEndingDirectorySeparator(
        [IO.Path]::GetFullPath([IO.Path]::GetTempPath()))
    if ([IO.Path]::GetDirectoryName($resolved) -cne $temporaryRoot `
            -or -not [IO.Path]::GetFileName($resolved).StartsWith(
                'dsr-release-redistributable-',
                [StringComparison]::Ordinal) `
            -or (([IO.File]::GetAttributes($resolved) `
                    -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Unsafe test cleanup target: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
