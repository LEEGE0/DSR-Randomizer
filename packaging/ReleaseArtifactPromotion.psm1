Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'SafeReleaseDirectories.psm1')

function Assert-RegularArtifactFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $attributes = [IO.File]::GetAttributes($Path)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description is a reparse point: $Path"
    }
}

function Set-ReleaseTransactionState {
    param(
        [Parameter(Mandatory = $true)][string]$TransactionRoot,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$State
    )

    $statePath = Join-Path $TransactionRoot 'transaction-state.json'
    $temporaryStatePath = Join-Path $TransactionRoot 'transaction-state.next'
    $json = $State | ConvertTo-Json -Compress -Depth 4
    [IO.File]::WriteAllText(
        $temporaryStatePath,
        $json,
        [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporaryStatePath, $statePath, $true)
}

function Invoke-PromotionHook {
    param(
        [Parameter(Mandatory = $false)][scriptblock]$BeforeFileMove,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][int]$Index,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -ne $BeforeFileMove) {
        & $BeforeFileMove $Phase $Index $Name
    }
}

function Publish-ReleaseArtifactSet {
    param(
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][ValidateCount(4, 4)][string[]]$ArtifactNames,
        [Parameter(Mandatory = $false)][scriptblock]$BeforeFileMove
    )

    $names = [Collections.Generic.List[string]]::new()
    $nameSet = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $ArtifactNames) {
        if ([string]::IsNullOrWhiteSpace($name) `
                -or [IO.Path]::IsPathRooted($name) `
                -or [IO.Path]::GetFileName($name) -cne $name `
                -or $name.Contains([IO.Path]::DirectorySeparatorChar) `
                -or $name.Contains([IO.Path]::AltDirectorySeparatorChar)) {
            throw "Release artifact name is not a plain file name: $name"
        }
        if (-not $nameSet.Add($name)) {
            throw "Release artifact names are not unique: $name"
        }
        $names.Add($name)
    }

    $stagingDirectory = Open-SafeReleaseRoot -Path $StagingRoot
    $outputDirectory = $null
    $transaction = $null
    try {
        $outputDirectory = Open-SafeReleaseRoot -Path $OutputRoot
        if ($stagingDirectory.Path.Equals(
                $outputDirectory.Path,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Release staging and output roots must be different directories.'
        }

        $staleTransactions = @(Get-ChildItem `
            -LiteralPath $outputDirectory.Path `
            -Directory `
            -Force `
            -Filter 'release-publish-transaction-*')
        if ($staleTransactions.Count -ne 0) {
            throw "A stale release publication transaction requires manual recovery: $($staleTransactions.FullName -join ', ')"
        }

        $priorNames = [Collections.Generic.List[string]]::new()
        foreach ($name in $names) {
            $stagedPath = Join-Path $stagingDirectory.Path $name
            Assert-RegularArtifactFile `
                -Path $stagedPath `
                -Description 'Staged release artifact'

            $outputPath = Join-Path $outputDirectory.Path $name
            if (Test-Path -LiteralPath $outputPath) {
                Assert-RegularArtifactFile `
                    -Path $outputPath `
                    -Description 'Prior release artifact'
                $priorNames.Add($name)
            }
        }
        if ($priorNames.Count -ne 0 -and $priorNames.Count -ne $names.Count) {
            throw "The output contains a partial prior release artifact set ($($priorNames.Count) of $($names.Count)); publication is fail-closed."
        }
        $hasPriorSet = $priorNames.Count -eq $names.Count

        $transaction = New-SafeReleaseDirectory `
            -TrustedRoot $outputDirectory.Path `
            -LeafPrefix 'release-publish-transaction-'
        $state = [ordered]@{
            schemaVersion = 1
            phase = 'staging'
            hadPriorSet = $hasPriorSet
            artifactNames = @($names)
            backedUp = @()
            promoted = @()
        }
        Set-ReleaseTransactionState `
            -TransactionRoot $transaction.Path `
            -State $state

        $stagedHashes = [ordered]@{}
        for ($index = 0; $index -lt $names.Count; $index++) {
            $name = $names[$index]
            $source = Join-Path $stagingDirectory.Path $name
            $destination = Join-Path $transaction.Path ("new-$index")
            [IO.File]::Copy($source, $destination, $false)
            Assert-RegularArtifactFile `
                -Path $destination `
                -Description 'Destination-local staged release artifact'
            $sourceLength = [IO.FileInfo]::new($source).Length
            $destinationLength = [IO.FileInfo]::new($destination).Length
            $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
            $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
            if ($sourceLength -ne $destinationLength -or $sourceHash -cne $destinationHash) {
                throw "Destination-local staging verification failed for release artifact: $name"
            }
            $stagedHashes[$name] = $sourceHash
        }

        $backedUp = [Collections.Generic.List[string]]::new()
        $promoted = [Collections.Generic.List[string]]::new()
        try {
            $state.phase = 'backing-up'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
            if ($hasPriorSet) {
                for ($index = 0; $index -lt $names.Count; $index++) {
                    $name = $names[$index]
                    Invoke-PromotionHook `
                        -BeforeFileMove $BeforeFileMove `
                        -Phase 'Backup' `
                        -Index $index `
                        -Name $name
                    [IO.File]::Move(
                        (Join-Path $outputDirectory.Path $name),
                        (Join-Path $transaction.Path ("previous-$index")),
                        $false)
                    $backedUp.Add($name)
                    $state.backedUp = @($backedUp)
                    Set-ReleaseTransactionState `
                        -TransactionRoot $transaction.Path `
                        -State $state
                }
            }

            $state.phase = 'promoting'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
            for ($index = 0; $index -lt $names.Count; $index++) {
                $name = $names[$index]
                Invoke-PromotionHook `
                    -BeforeFileMove $BeforeFileMove `
                    -Phase 'Promote' `
                    -Index $index `
                    -Name $name
                [IO.File]::Move(
                    (Join-Path $transaction.Path ("new-$index")),
                    (Join-Path $outputDirectory.Path $name),
                    $false)
                $promoted.Add($name)
                $state.promoted = @($promoted)
                Set-ReleaseTransactionState `
                    -TransactionRoot $transaction.Path `
                    -State $state
            }

            foreach ($name in $names) {
                $outputPath = Join-Path $outputDirectory.Path $name
                Assert-RegularArtifactFile `
                    -Path $outputPath `
                    -Description 'Published release artifact'
                $outputHash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash
                if ($outputHash -cne $stagedHashes[$name]) {
                    throw "Published release artifact verification failed: $name"
                }
            }
            $state.phase = 'complete'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
        }
        catch {
            $publicationFailure = $_.Exception
            $rollbackFailures = [Collections.Generic.List[string]]::new()
            $state.phase = 'rolling-back'
            try {
                Set-ReleaseTransactionState `
                    -TransactionRoot $transaction.Path `
                    -State $state
            }
            catch {
                $rollbackFailures.Add("Unable to journal rollback: $($_.Exception.Message)")
            }

            for ($index = $promoted.Count - 1; $index -ge 0; $index--) {
                $name = $promoted[$index]
                try {
                    Invoke-PromotionHook `
                        -BeforeFileMove $BeforeFileMove `
                        -Phase 'RollbackRemove' `
                        -Index $index `
                        -Name $name
                    $outputPath = Join-Path $outputDirectory.Path $name
                    Assert-RegularArtifactFile `
                        -Path $outputPath `
                        -Description 'Newly promoted release artifact'
                    [IO.File]::Delete($outputPath)
                }
                catch {
                    $rollbackFailures.Add("Unable to remove newly promoted '$name': $($_.Exception.Message)")
                }
            }
            for ($index = $backedUp.Count - 1; $index -ge 0; $index--) {
                $name = $backedUp[$index]
                try {
                    Invoke-PromotionHook `
                        -BeforeFileMove $BeforeFileMove `
                        -Phase 'RollbackRestore' `
                        -Index $index `
                        -Name $name
                    [IO.File]::Move(
                        (Join-Path $transaction.Path ("previous-$index")),
                        (Join-Path $outputDirectory.Path $name),
                        $false)
                }
                catch {
                    $rollbackFailures.Add("Unable to restore prior '$name': $($_.Exception.Message)")
                }
            }

            if ($rollbackFailures.Count -ne 0) {
                $transaction.Lease.Dispose()
                $transaction.Lease = $null
                $retainedTransactionPath = $transaction.Path
                $transaction = $null
                throw [IO.IOException]::new(
                    "Release artifact publication failed and rollback was incomplete. " +
                    "Transaction retained at '$retainedTransactionPath'. " +
                    "Publication failure: $($publicationFailure.Message) " +
                    "Rollback failures: $($rollbackFailures -join ' | ')",
                    $publicationFailure)
            }

            $state.phase = 'rolled-back'
            Set-ReleaseTransactionState `
                -TransactionRoot $transaction.Path `
                -State $state
            Remove-SafeReleaseDirectory -Directory $transaction
            $transaction = $null
            throw [IO.IOException]::new(
                "Release artifact publication failed and the prior complete set was restored: $($publicationFailure.Message)",
                $publicationFailure)
        }

        Remove-SafeReleaseDirectory -Directory $transaction
        $transaction = $null
    }
    finally {
        if ($null -ne $transaction -and $null -ne $transaction.Lease) {
            $transaction.Lease.Dispose()
        }
        if ($null -ne $outputDirectory) {
            $outputDirectory.Lease.Dispose()
        }
        $stagingDirectory.Lease.Dispose()
    }
}

Export-ModuleMember -Function Publish-ReleaseArtifactSet
