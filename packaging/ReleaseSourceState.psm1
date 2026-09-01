Set-StrictMode -Version Latest

function Invoke-ReleaseStateGit {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage,
        [switch]$PreserveLeadingWhitespace
    )

    $output = @(& git -C $WorkingDirectory @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage Exit code: $LASTEXITCODE`n$($output -join [Environment]::NewLine)"
    }
    $text = $output -join [Environment]::NewLine
    if ($PreserveLeadingWhitespace) {
        return $text.TrimEnd()
    }
    return $text.Trim()
}

function Get-ReleaseSourceContract {
    [CmdletBinding()]
    param()

    return [ordered]@{
        'third_party/SoulsFormatsNEXT' = '55b08a3c02a03777cf19958d8f6aa18d7af59da1'
        'third_party/ZstdNet' = 'c90152918f633e945f163652e6368001556784e7'
        'third_party/zstd' = 'b706286adbba780006a47ef92df0ad7a785666b6'
    }
}

function Assert-ReleaseSourceState {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$RequiredSubmodules
    )

    $root = [IO.Path]::GetFullPath($RepositoryRoot)
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Main repository is missing: $root"
    }

    $mainRevision = Invoke-ReleaseStateGit `
        -WorkingDirectory $root `
        -Arguments @('rev-parse', '--verify', 'HEAD^{commit}') `
        -FailureMessage 'Main repository HEAD is not a committed revision.'
    $mainStatus = Invoke-ReleaseStateGit `
        -WorkingDirectory $root `
        -Arguments @('status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=all') `
        -FailureMessage 'Unable to inspect main repository state.'
    if (-not [string]::IsNullOrWhiteSpace($mainStatus)) {
        throw "Main repository has tracked or nonignored untracked changes:`n$mainStatus"
    }

    $statusOutput = Invoke-ReleaseStateGit `
        -WorkingDirectory $root `
        -Arguments @('submodule', 'status', '--recursive') `
        -FailureMessage 'Unable to inspect recursive submodule state.' `
        -PreserveLeadingWhitespace
    $submodules = [ordered]@{}
    foreach ($line in @($statusOutput -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
        $match = [regex]::Match(
            $line,
            '^(?<state>[ +\-U])(?<commit>[0-9a-fA-F]{40}) (?<path>.+?)(?: \(.+\))?$')
        if (-not $match.Success) {
            throw "Unable to parse recursive submodule state: $line"
        }

        $state = $match.Groups['state'].Value
        $commit = $match.Groups['commit'].Value.ToLowerInvariant()
        $path = $match.Groups['path'].Value.Replace('\', '/')
        switch ($state) {
            '-' { throw "Submodule '$path' is not initialized." }
            '+' { throw "Submodule '$path' is at the wrong commit: $commit" }
            'U' { throw "Submodule '$path' has an unresolved gitlink conflict." }
        }
        $submodules[$path] = $commit
    }

    foreach ($entry in $RequiredSubmodules.GetEnumerator()) {
        $path = ([string]$entry.Key).Replace('\', '/')
        $expectedRevision = ([string]$entry.Value).ToLowerInvariant()
        if (-not $submodules.Contains($path)) {
            throw "Required submodule '$path' is missing from the recursive gitlink state."
        }

        $actualRevision = [string]$submodules[$path]
        if ($actualRevision -cne $expectedRevision) {
            throw "Submodule '$path' is at the wrong commit: $actualRevision; expected $expectedRevision."
        }
    }

    foreach ($entry in $submodules.GetEnumerator()) {
        $path = [string]$entry.Key
        $submoduleRoot = [IO.Path]::GetFullPath((Join-Path $root $path))
        if (-not (Test-Path -LiteralPath $submoduleRoot -PathType Container)) {
            throw "Submodule '$path' is not initialized: checkout directory is missing."
        }

        $actualRevision = Invoke-ReleaseStateGit `
            -WorkingDirectory $submoduleRoot `
            -Arguments @('rev-parse', '--verify', 'HEAD^{commit}') `
            -FailureMessage "Submodule '$path' HEAD is unavailable."
        if ($actualRevision -cne [string]$entry.Value) {
            throw "Submodule '$path' is at the wrong commit: $actualRevision; expected $($entry.Value)."
        }

        $submoduleStatus = Invoke-ReleaseStateGit `
            -WorkingDirectory $submoduleRoot `
            -Arguments @('status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=all') `
            -FailureMessage "Unable to inspect submodule '$path' state."
        if (-not [string]::IsNullOrWhiteSpace($submoduleStatus)) {
            throw "Submodule '$path' has tracked or nonignored untracked changes:`n$submoduleStatus"
        }
    }

    return [pscustomobject]@{
        MainRevision = $mainRevision
        Submodules = $submodules
    }
}

Export-ModuleMember -Function Get-ReleaseSourceContract, Assert-ReleaseSourceState
