$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$modulePath = Join-Path $PSScriptRoot '..\SafeReleaseDirectories.psm1'
Import-Module $modulePath -Force

function Assert-Throws {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [string]$ExpectedMessage = ''
    )

    try {
        & $Action
    }
    catch {
        if (-not [string]::IsNullOrEmpty($ExpectedMessage) `
            -and -not $_.Exception.Message.Contains(
            $ExpectedMessage,
            [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected failure. Wanted '$ExpectedMessage', got '$($_.Exception.Message)'."
        }
        return
    }
    throw "The action did not reject: $ExpectedMessage"
}

function New-Junction {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $start = [Diagnostics.ProcessStartInfo]::new('cmd.exe')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('/d', '/c', 'mklink', '/J', $Path, $Target)) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    if ($null -eq $process) {
        throw 'Unable to start the junction helper.'
    }
    $output = $process.StandardOutput.ReadToEnd()
    $errorOutput = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "Junction creation failed: $output $errorOutput"
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'dsr-safe-release-directories-' + [Guid]::NewGuid().ToString('N'))
$trustedRoot = Join-Path $testRoot 'trusted'
$outsideRoot = Join-Path $testRoot 'outside'
$rootJunction = Join-Path $testRoot 'root-junction'
$childJunction = $null
$generated = $null
try {
    [IO.Directory]::CreateDirectory($trustedRoot) | Out-Null
    [IO.Directory]::CreateDirectory($outsideRoot) | Out-Null
    $outsideSentinel = Join-Path $outsideRoot 'outside-sentinel.txt'
    [IO.File]::WriteAllText($outsideSentinel, 'must survive')

    New-Junction -Path $rootJunction -Target $outsideRoot
    Assert-Throws -ExpectedMessage 'reparse' -Action {
        New-SafeReleaseDirectory `
            -TrustedRoot $rootJunction `
            -LeafPrefix 'release-work-test-' | Out-Null
    }
    if (-not (Test-Path -LiteralPath $outsideSentinel -PathType Leaf)) {
        throw 'Rejecting a junctioned trusted root deleted the outside sentinel.'
    }

    $generated = New-SafeReleaseDirectory `
        -TrustedRoot $trustedRoot `
        -LeafPrefix 'package-staging-test-'
    $movedPath = "$($generated.Path)-moved"
    Assert-Throws -Action {
        [IO.Directory]::Move($generated.Path, $movedPath)
    }

    $childJunction = Join-Path $generated.Path 'redirected-child'
    New-Junction -Path $childJunction -Target $outsideRoot
    Assert-Throws -ExpectedMessage 'reparse' -Action {
        Remove-SafeReleaseDirectory -Directory $generated
    }
    if (-not (Test-Path -LiteralPath $outsideSentinel -PathType Leaf)) {
        throw 'Rejecting a junctioned cleanup target deleted the outside sentinel.'
    }

    [IO.Directory]::Delete($childJunction)
    $childJunction = $null
    Remove-SafeReleaseDirectory -Directory $generated
    $generated = $null
    if (Test-Path -LiteralPath $movedPath) {
        throw 'The leased generated directory was renamed during verification.'
    }
}
finally {
    if ($null -ne $generated) {
        $generated.Lease.Dispose()
    }
    if ($null -ne $childJunction -and (Test-Path -LiteralPath $childJunction)) {
        [IO.Directory]::Delete($childJunction)
    }
    if (Test-Path -LiteralPath $rootJunction) {
        [IO.Directory]::Delete($rootJunction)
    }
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTempRoot = [IO.Path]::TrimEndingDirectorySeparator(
        [IO.Path]::GetFullPath([IO.Path]::GetTempPath()))
    if ($resolvedTestRoot.StartsWith(
        $resolvedTempRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase) `
        -and [IO.Path]::GetFileName($resolvedTestRoot).StartsWith(
            'dsr-safe-release-directories-',
            [StringComparison]::Ordinal) `
        -and (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
