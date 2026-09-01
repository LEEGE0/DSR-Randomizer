$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$modulePath = Join-Path $PSScriptRoot '..\ReleaseRedistributable.psm1'
Import-Module $modulePath -Force

$expectedExports = @(
    'Assert-ReleaseRedistributableArchive',
    'New-ReleaseRedistributableArchive',
    'Publish-ReleaseRedistributableArchive',
    'Remove-LegacyReleaseArtifacts'
)
$actualExports = @(
    (Get-Module ReleaseRedistributable).ExportedCommands.Keys |
        Sort-Object -CaseSensitive)
if (@(Compare-Object $expectedExports $actualExports -SyncWindow 0).Count -ne 0) {
    throw "Unexpected redistributable module exports: $($actualExports -join ', ')"
}

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
            $_.Name.Contains('.pending-', [StringComparison]::Ordinal)
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

    # The final hook before the handle rename is a pre-commit boundary. A
    # failure there must preserve the old canonical or first-publish absence.
    $preFailureCase = New-Case -Root $testRoot -Name 'handle-rename-failure'
    $preOld = New-OuterFixture -Case $preFailureCase -Prefix 'old'
    Publish-Fixture -Case $preFailureCase -Fixture $preOld
    $preNewRoot = Join-Path $preFailureCase.StagingRoot 'new'
    $preNew = New-OuterFixture -Case $preFailureCase -Prefix 'new' -StagingRoot $preNewRoot
    Assert-Failed `
        -Message 'controlled handle rename failure' `
        -Action {
            Publish-Fixture `
                -Case $preFailureCase `
                -Fixture $preNew `
                -OperationHook {
                    param($Phase, $Path)
                    if ($Phase -eq 'BeforeHandleRename') {
                        throw 'controlled handle rename failure'
                    }
                }
        }
    Assert-BytesEqual `
        -Expected $preOld.Bytes `
        -Path (Join-Path $preFailureCase.OutputRoot $outerName)
    Assert-NoPublicationResidue -OutputRoot $preFailureCase.OutputRoot

    $firstPreFailureCase = New-Case -Root $testRoot -Name 'first-handle-rename-failure'
    $firstPreFailure = New-OuterFixture -Case $firstPreFailureCase -Prefix 'new'
    Assert-Failed `
        -Message 'controlled first handle rename failure' `
        -Action {
            Publish-Fixture `
                -Case $firstPreFailureCase `
                -Fixture $firstPreFailure `
                -OperationHook {
                    param($Phase, $Path)
                    if ($Phase -eq 'BeforeHandleRename') {
                        throw 'controlled first handle rename failure'
                    }
                }
        }
    if (Test-Path -LiteralPath (Join-Path $firstPreFailureCase.OutputRoot $outerName)) {
        throw 'Failed first handle rename created a canonical archive.'
    }
    Assert-NoPublicationResidue -OutputRoot $firstPreFailureCase.OutputRoot

    # Exercise a real handle-rename failure: the hook installs and leases a
    # temporary destination blocker, while the candidate remains valid.
    $firstRenameFailureCase = New-Case `
        -Root $testRoot `
        -Name 'first-actual-handle-rename-failure'
    $firstRenameFailure = New-OuterFixture `
        -Case $firstRenameFailureCase `
        -Prefix 'new'
    $firstRenameDestination = Join-Path `
        $firstRenameFailureCase.OutputRoot `
        $outerName
    $script:renameBlocker = $null
    Assert-Failed `
        -Message 'publication rename failed' `
        -Action {
            try {
                Publish-Fixture `
                    -Case $firstRenameFailureCase `
                    -Fixture $firstRenameFailure `
                    -OperationHook {
                        param($Phase, $Path)
                        if ($Phase -eq 'BeforeHandleRename') {
                            [IO.File]::WriteAllText(
                                $firstRenameDestination,
                                'rename blocker',
                                [Text.UTF8Encoding]::new($false))
                            $script:renameBlocker = [IO.FileStream]::new(
                                $firstRenameDestination,
                                [IO.FileMode]::Open,
                                [IO.FileAccess]::Read,
                                [IO.FileShare]::Read)
                        }
                    }
            }
            finally {
                if ($null -ne $script:renameBlocker) {
                    $script:renameBlocker.Dispose()
                    $script:renameBlocker = $null
                }
                if (Test-Path -LiteralPath $firstRenameDestination) {
                    [IO.File]::Delete($firstRenameDestination)
                }
            }
        }
    if (Test-Path -LiteralPath $firstRenameDestination) {
        throw 'Actual first handle-rename failure left a canonical archive.'
    }
    Assert-NoPublicationResidue -OutputRoot $firstRenameFailureCase.OutputRoot

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
            if ($Phase -eq 'BeforeHandleRename') {
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

    # A hostile extra hard link added by the final pre-rename hook must be
    # caught through the still-open candidate handle before commit.
    $hardLinkCase = New-Case -Root $testRoot -Name 'pending-hardlink-existing'
    $hardLinkOld = New-OuterFixture -Case $hardLinkCase -Prefix 'old'
    Publish-Fixture -Case $hardLinkCase -Fixture $hardLinkOld
    $hardLinkNewRoot = Join-Path $hardLinkCase.StagingRoot 'new'
    $hardLinkNew = New-OuterFixture `
        -Case $hardLinkCase `
        -Prefix 'new' `
        -StagingRoot $hardLinkNewRoot
    $hardLinkAlias = Join-Path $hardLinkCase.OutputRoot 'hostile-pending-alias.zip'
    $script:pendingHardLinkCreated = $false
    Assert-Failed `
        -Message 'multiple hard links' `
        -Action {
            try {
                Publish-Fixture `
                    -Case $hardLinkCase `
                    -Fixture $hardLinkNew `
                    -OperationHook {
                        param($Phase, $Path)
                        if ($Phase -eq 'BeforeHandleRename') {
                            New-Item `
                                -ItemType HardLink `
                                -Path $hardLinkAlias `
                                -Target $Path | Out-Null
                            $script:pendingHardLinkCreated = $true
                        }
                    }
            }
            finally {
                if (Test-Path -LiteralPath $hardLinkAlias) {
                    [IO.File]::Delete($hardLinkAlias)
                }
            }
        }
    if (-not $script:pendingHardLinkCreated) {
        throw 'The pre-rename hard-link fixture was not created.'
    }
    Assert-BytesEqual `
        -Expected $hardLinkOld.Bytes `
        -Path (Join-Path $hardLinkCase.OutputRoot $outerName)
    Assert-NoPublicationResidue -OutputRoot $hardLinkCase.OutputRoot

    $firstHardLinkCase = New-Case -Root $testRoot -Name 'pending-hardlink-first'
    $firstHardLinkFixture = New-OuterFixture `
        -Case $firstHardLinkCase `
        -Prefix 'new'
    $firstHardLinkAlias = Join-Path `
        $firstHardLinkCase.OutputRoot `
        'hostile-first-pending-alias.zip'
    Assert-Failed `
        -Message 'multiple hard links' `
        -Action {
            try {
                Publish-Fixture `
                    -Case $firstHardLinkCase `
                    -Fixture $firstHardLinkFixture `
                    -OperationHook {
                        param($Phase, $Path)
                        if ($Phase -eq 'BeforeHandleRename') {
                            New-Item `
                                -ItemType HardLink `
                                -Path $firstHardLinkAlias `
                                -Target $Path | Out-Null
                        }
                    }
            }
            finally {
                if (Test-Path -LiteralPath $firstHardLinkAlias) {
                    [IO.File]::Delete($firstHardLinkAlias)
                }
            }
        }
    if (Test-Path -LiteralPath (Join-Path $firstHardLinkCase.OutputRoot $outerName)) {
        throw 'First pre-rename hard-link failure left a canonical archive.'
    }
    Assert-NoPublicationResidue -OutputRoot $firstHardLinkCase.OutputRoot

    # The handle rename is the commit. Post-commit hook failure cannot roll
    # back or let another pathname replace the live expected canonical.
    $finalSwapCase = New-Case -Root $testRoot -Name 'post-commit-path-swap'
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
    $script:finalSwapHookRan = $false
    Publish-Fixture `
        -Case $finalSwapCase `
        -Fixture $finalNew `
        -OperationHook {
            param($Phase, $Path)
            if ($Phase -eq 'AfterHandleRename') {
                $script:finalSwapHookRan = $true
                $canonical = Join-Path $finalSwapCase.OutputRoot $outerName
                try {
                    [IO.File]::Copy($finalThird.Path, $canonical, $true)
                }
                catch [IO.IOException] {
                    $script:finalSwapBlocked = $true
                }
                throw 'controlled post-commit cleanup failure'
            }
        }
    if (-not $script:finalSwapHookRan) {
        throw 'The post-commit hook did not run.'
    }
    if (-not $script:finalSwapBlocked) {
        throw 'Canonical replacement was not blocked through the success boundary.'
    }
    Assert-BytesEqual `
        -Expected $finalNew.Bytes `
        -Path (Join-Path $finalSwapCase.OutputRoot $outerName)
    Assert-NoPublicationResidue -OutputRoot $finalSwapCase.OutputRoot

    # A hard link created after commit but before the mandatory same-handle
    # safety check must fail closed by removing the canonical link.
    $postLinkCase = New-Case -Root $testRoot -Name 'post-rename-hardlink'
    $postLinkFixture = New-OuterFixture -Case $postLinkCase -Prefix 'new'
    $postLinkAlias = Join-Path $postLinkCase.OutputRoot 'hostile-final-alias.zip'
    $script:postLinkCreated = $false
    Assert-Failed `
        -Message 'multiple hard links' `
        -Action {
            try {
                Publish-Fixture `
                    -Case $postLinkCase `
                    -Fixture $postLinkFixture `
                    -OperationHook {
                        param($Phase, $Path)
                        if ($Phase -eq 'AfterHandleRenameBeforeVerification') {
                            New-Item `
                                -ItemType HardLink `
                                -Path $postLinkAlias `
                                -Target $Path | Out-Null
                            $script:postLinkCreated = $true
                        }
                    }
            }
            finally {
                if (Test-Path -LiteralPath $postLinkAlias) {
                    [IO.File]::Delete($postLinkAlias)
                }
            }
        }
    if (-not $script:postLinkCreated) {
        throw 'The post-rename hard-link fixture was not created.'
    }
    if (Test-Path -LiteralPath (Join-Path $postLinkCase.OutputRoot $outerName)) {
        throw 'Post-rename hard-link failure left an authoritative canonical.'
    }
    Assert-NoPublicationResidue -OutputRoot $postLinkCase.OutputRoot

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
