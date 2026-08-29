param(
    [Parameter(Mandatory = $true)][string]$ExternalRoot,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$externalRootPath = [IO.Path]::GetFullPath($ExternalRoot).TrimEnd('\')
if (-not (Test-Path -LiteralPath $externalRootPath -PathType Container)) {
    throw "External root does not exist: $externalRootPath"
}

function Read-StrictJson([string]$Path) {
    $information = Get-Item -LiteralPath $Path
    if ($information.Length -gt 65536) {
        throw "Configuration is oversized: $Path"
    }
    return [IO.File]::ReadAllText($Path) | ConvertFrom-Json
}

function Resolve-DeploymentState {
    $pointer = Read-StrictJson (Join-Path $externalRootPath 'runtime-current.json')
    if ($pointer.runtimeId -notmatch '^runtime-[0-9a-f]{8,128}$') {
        throw 'runtime-current.json contains an invalid runtime ID.'
    }
    $runtimeRoot = [IO.Path]::GetFullPath(
        (Join-Path $externalRootPath ([string]$pointer.relativeRuntimePath)))
    $expectedRuntimeParent = [IO.Path]::GetFullPath((Join-Path $externalRootPath 'runtimes'))
    $runtimeBelowRoot = $runtimeRoot.StartsWith(
        $expectedRuntimeParent + '\', [StringComparison]::OrdinalIgnoreCase)
    $runtimeLeafMatches = [IO.Path]::GetFileName($runtimeRoot) -ceq [string]$pointer.runtimeId
    if (-not $runtimeBelowRoot -or -not $runtimeLeafMatches) {
        throw 'The active runtime pointer escapes or mismatches the runtimes directory.'
    }
    $selection = Read-StrictJson (Join-Path $externalRootPath 'config\selected-save-profile.json')
    if ([string]$selection.steamId -notmatch '^[0-9]{1,20}$') {
        throw 'The selected save profile contains an invalid Steam ID.'
    }
    $saveRoot = Join-Path $externalRootPath "saves\$($selection.steamId)"
    $rmmPath = Join-Path $saveRoot 'DRAKS0005.rmm'
    $metadata = Read-StrictJson (Join-Path $saveRoot 'save-metadata.json')
    $rmm = Get-Item -LiteralPath $rmmPath
    $rmmHash = (Get-FileHash -LiteralPath $rmmPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $metadataValid = $metadata.schemaVersion -eq 1 `
        -and [string]$metadata.steamId -ceq [string]$selection.steamId `
        -and $metadata.fixedLength -eq 4326608 `
        -and $rmm.Length -eq 4326608 `
        -and [string]$metadata.lastKnownSha256 -ceq $rmmHash
    if (-not $metadataValid) {
        throw 'The selected RMM and save metadata are not a clean matching pair.'
    }
    $tomlFiles = @(Get-ChildItem -LiteralPath (Join-Path $runtimeRoot 'Mods') `
        -Filter 'config_randomizer.toml' -File -Recurse | Where-Object {
            $_.Directory.Name -eq 'DS1EnemyRandomizer'
        })
    if ($tomlFiles.Count -ne 1) {
        throw "Expected exactly one enemy-randomizer TOML, found $($tomlFiles.Count)."
    }
    return [pscustomobject]@{
        RuntimeRoot = $runtimeRoot
        RuntimeId = [string]$pointer.runtimeId
        SteamId = [string]$selection.steamId
        RmmPath = $rmmPath
        RmmHash = $rmmHash
        TomlPath = $tomlFiles[0].FullName
    }
}

function Assert-BridgeExport([string]$DllPath) {
    $handle = [Runtime.InteropServices.NativeLibrary]::Load($DllPath)
    try {
        $export = [Runtime.InteropServices.NativeLibrary]::GetExport(
            $handle, 'modengine_ext_init')
        if ($export -eq [IntPtr]::Zero) {
            throw 'The bridge DLL does not export modengine_ext_init.'
        }
    }
    finally {
        [Runtime.InteropServices.NativeLibrary]::Free($handle)
    }
}

function Assert-Deployed([pscustomobject]$State) {
    $deploymentRoot = Join-Path $externalRootPath 'components\rmm-bridge'
    $dllPath = Join-Path $deploymentRoot 'DSRRandomizer.RmmBridge.dll'
    $hostPath = Join-Path $deploymentRoot 'DSRRandomizer.RmmBridgeHost.exe'
    $manifestPath = Join-Path $deploymentRoot 'deployment-manifest.json'
    foreach ($path in @($dllPath, $hostPath, $manifestPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "A deployed bridge artifact is missing: $path"
        }
    }
    Assert-BridgeExport $dllPath
    $manifest = Read-StrictJson $manifestPath
    $dllHash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $hostHash = (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifestMatches = [string]$manifest.bridgeSha256 -ceq $dllHash `
        -and [string]$manifest.hostSha256 -ceq $hostHash
    if (-not $manifestMatches) {
        throw 'Deployed artifact hashes do not match deployment-manifest.json.'
    }
    $toml = [IO.File]::ReadAllText($State.TomlPath)
    $escapedDll = $dllPath.Replace('\', '\\')
    $tomlHasBridge = $toml.Contains(
        '"' + $escapedDll + '"', [StringComparison]::Ordinal)
    $tomlHasItemExecutable = $toml.Contains(
        'DarkSoulsItemRandomizer.exe', [StringComparison]::OrdinalIgnoreCase)
    $tomlHasHeapPatch = $toml.Contains(
        'DS1HeapPatch.dll', [StringComparison]::OrdinalIgnoreCase)
    if (-not $tomlHasBridge -or -not $tomlHasHeapPatch -or $tomlHasItemExecutable) {
        throw 'Enemy-randomizer TOML does not contain the bridge-only external DLL list.'
    }
    $currentRmmHash = (Get-FileHash -LiteralPath $State.RmmPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($currentRmmHash -cne $State.RmmHash) {
        throw 'The RMM changed during bridge publish or verification.'
    }
    return [pscustomobject]@{
        DeploymentRoot = $deploymentRoot
        BridgeSha256 = $dllHash
        HostSha256 = $hostHash
        TomlPath = $State.TomlPath
        RmmSha256 = $currentRmmHash
    }
}

$state = Resolve-DeploymentState
if ($VerifyOnly) {
    Assert-Deployed $state | Format-List
    exit 0
}

$running = @(Get-Process -Name 'DarkSoulsRemastered','DSRRandomizer.RmmBridgeHost' -ErrorAction SilentlyContinue)
if ($running.Count -ne 0) {
    throw 'Close Dark Souls Remastered and the RMM bridge host before deployment.'
}

$preset = if ($Configuration -eq 'Release') { 'windows-x64-release' } else { 'windows-x64-debug' }
Push-Location $repositoryRoot
try {
    & cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & cmake --build --preset $preset --target DSRRandomizer.RmmBridge
    if ($LASTEXITCODE -ne 0) { throw 'Native bridge build failed.' }
}
finally {
    Pop-Location
}

$buildDll = Join-Path $repositoryRoot "native\out\build\$preset\native\runtime\$Configuration\DSRRandomizer.RmmBridge.dll"
$stageRoot = Join-Path $externalRootPath ("staging\rmm-bridge-" + [Guid]::NewGuid().ToString('N'))
$hostStage = Join-Path $stageRoot 'host'
New-Item -ItemType Directory -Path $hostStage | Out-Null
try {
    $publishArguments = @(
        'publish',
        (Join-Path $repositoryRoot 'src\DSRRandomizer.RmmBridgeHost\DSRRandomizer.RmmBridgeHost.csproj'),
        '-c', $Configuration,
        '-r', 'win-x64',
        '--self-contained', 'true',
        '-p:PublishSingleFile=true',
        '-p:IncludeNativeLibrariesForSelfExtract=true',
        '-o', $hostStage)
    & dotnet @publishArguments
    if ($LASTEXITCODE -ne 0) { throw 'Managed bridge-host publish failed.' }
    $buildHost = Join-Path $hostStage 'DSRRandomizer.RmmBridgeHost.exe'
    Assert-BridgeExport $buildDll
    $bridgeHash = (Get-FileHash -LiteralPath $buildDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $hostHash = (Get-FileHash -LiteralPath $buildHost -Algorithm SHA256).Hash.ToLowerInvariant()

    $deploymentRoot = Join-Path $externalRootPath 'components\rmm-bridge'
    New-Item -ItemType Directory -Path $deploymentRoot -Force | Out-Null
    Copy-Item -LiteralPath $buildDll -Destination (Join-Path $deploymentRoot 'DSRRandomizer.RmmBridge.dll') -Force
    Copy-Item -LiteralPath $buildHost -Destination (Join-Path $deploymentRoot 'DSRRandomizer.RmmBridgeHost.exe') -Force
    $manifest = [ordered]@{
        schemaVersion = 1
        configuration = $Configuration
        runtimeId = $state.RuntimeId
        bridgeSha256 = $bridgeHash
        hostSha256 = $hostHash
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $deploymentRoot 'deployment-manifest.json'), $manifest)

    $toml = [IO.File]::ReadAllText($state.TomlPath)
    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    $backupPath = "$($state.TomlPath).pre-rmm-bridge-$timestamp.bak"
    [IO.File]::WriteAllBytes($backupPath, [IO.File]::ReadAllBytes($state.TomlPath))
    $toml = [Regex]::Replace(
        $toml,
        '(?m)^\s*"[^"]*DarkSoulsItemRandomizer\.exe",?\s*\r?\n',
        '')
    $escapedDll = (Join-Path $deploymentRoot 'DSRRandomizer.RmmBridge.dll').Replace('\', '\\')
    if (-not $toml.Contains($escapedDll, [StringComparison]::Ordinal)) {
        $toml = [Regex]::Replace(
            $toml,
            '(?m)^(external_dlls\s*=\s*\[\s*)$',
            "`$1`r`n    `"$escapedDll`",")
    }
    [IO.File]::WriteAllText($state.TomlPath, $toml, [Text.UTF8Encoding]::new($false))
}
finally {
    $resolvedStage = [IO.Path]::GetFullPath($stageRoot)
    $resolvedStagingRoot = [IO.Path]::GetFullPath((Join-Path $externalRootPath 'staging'))
    $stageInsideRoot = $resolvedStage.StartsWith(
        $resolvedStagingRoot + '\', [StringComparison]::OrdinalIgnoreCase)
    $stageLeafSafe = [IO.Path]::GetFileName($resolvedStage).StartsWith(
        'rmm-bridge-', [StringComparison]::Ordinal)
    if ($stageInsideRoot -and $stageLeafSafe) {
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Assert-Deployed $state | Format-List
