# Runs every scenario in tools/test/scenarios (or those matching -Filter) and sums up.
#   tools/test/run_all.ps1 [-Filter boss*] [-Force]
param([string]$Filter = '*', [switch]$Force, [string]$Game = '')
$results = @()
foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot 'scenarios') -Filter "$Filter.txt" | Sort-Object Name) {
    Write-Host "`n#### $($f.Name)"
    & (Join-Path $PSScriptRoot 'run_scenario.ps1') -Scenario $f.FullName -Force:$Force -Game $Game
    $code = $LASTEXITCODE
    $results += [pscustomobject]@{ Scenario = $f.BaseName; Result = @('ok', 'FAILED', 'no build', 'PC in use')[[math]::Min($code, 3)] }
    if ($code -eq 3) { break }  # someone's at the PC: stop
}
Write-Host ''
$results | Format-Table -AutoSize
if ($results | Where-Object Result -ne 'ok') { exit 1 }
