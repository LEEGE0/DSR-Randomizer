param(
    [ValidateRange(1, 1000)]
    [int]$Count = 20,

    [string]$Preset = 'windows-x64-debug',

    [string]$CtestPath = 'ctest'
)

$ErrorActionPreference = 'Stop'

for ($run = 1; $run -le $Count; $run++) {
    & $CtestPath `
        --preset $Preset `
        --output-on-failure `
        -R '^SaveHookIntegrationTests$'
    if ($LASTEXITCODE -ne 0) {
        throw "SaveHookIntegrationTests failed on repetition $run of $Count"
    }
    Write-Output "SaveHookIntegrationTests repetition $run/$Count passed"
}
