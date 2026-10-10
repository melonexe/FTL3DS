# Runs a scenario in Azahar with the test build and reports what happened.
#
#   tools/test/run_scenario.ps1 tools/test/scenarios/boss1.txt [-Timeout 120] [-Force]
#
# Build the test build first: make TEST=1 (ftl3ds_test.3dsx). The scenario is copied to the
# emulated SD card as test.txt; the game runs its commands and writes test.log, which this script
# follows: each "SHOT <name>" line becomes a screenshot of the Azahar window in
# build_test/scenarios/<scenario>/<name>.png, and the run ends at DONE (or the timeout). The log is
# printed with a PASS / FAIL / ERROR count; the exit code is 1 if anything failed.
#
# Screenshots bring Azahar to the front, so unless -Force is given the script won't start while
# someone has used the PC in the last two minutes.
param(
    [Parameter(Mandatory = $true)][string]$Scenario,
    [int]$Timeout = 120,
    [switch]$Force,
    [string]$Game = ''   # a different test build (e.g. a snapshot), default ftl3ds_test.3dsx
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$sd = Join-Path $env:APPDATA 'Azahar\sdmc\3ds\ftl3ds'
$azahar = 'C:\Program Files\Azahar\azahar.exe'
$game = if ($Game) { $Game } else { Join-Path $root 'ftl3ds_test.3dsx' }
$name = [IO.Path]::GetFileNameWithoutExtension($Scenario)
$out = Join-Path $root "build_test\scenarios\$name"

Add-Type @"
using System; using System.Runtime.InteropServices;
public class ScenarioWin {
  [StructLayout(LayoutKind.Sequential)] public struct LII { public uint cb; public uint t; }
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LII l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, int f, int e);
  public struct R { public int L, T, Ri, B; }
}
"@
Add-Type -AssemblyName System.Drawing

# Our own key press (for the screenshots) counts as input to Windows: remember when we sent it, and
# don't mistake it for someone at the PC.
$tickFile = Join-Path $env:TEMP 'ftl3ds_runner_input.txt'
if (-not $Force) {
    $l = New-Object ScenarioWin+LII; $l.cb = 8; [ScenarioWin]::GetLastInputInfo([ref]$l) | Out-Null
    $idle = ([Environment]::TickCount - $l.t) / 1000
    $ours = (Test-Path $tickFile) -and ([long]$l.t -le [long](Get-Content $tickFile) + 1500)
    if ($idle -lt 120 -and -not $ours) { Write-Host "Someone used the PC $([int]$idle) s ago; not taking over the screen (use -Force)."; exit 3 }
}
if (-not (Test-Path $game)) { Write-Host "No $game - build it with: make TEST=1"; exit 2 }
if (-not (Test-Path $Scenario)) { Write-Host "No scenario $Scenario"; exit 2 }

New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter *.png -ErrorAction SilentlyContinue | Remove-Item
Copy-Item $Scenario (Join-Path $sd 'test.txt') -Force
$log = Join-Path $sd 'test.log'
Remove-Item $log -ErrorAction SilentlyContinue

function Shot([string]$file) {
    $p = Get-Process azahar -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
    if (-not $p) { return }
    $h = $p.MainWindowHandle
    [ScenarioWin]::keybd_event(0x12, 0, 0, 0); [ScenarioWin]::keybd_event(0x12, 0, 2, 0)  # Alt: lets us take focus
    ([long][Environment]::TickCount -band 0xFFFFFFFFL) | Set-Content $tickFile
    [ScenarioWin]::ShowWindow($h, 9) | Out-Null; [ScenarioWin]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 250
    $r = New-Object ScenarioWin+R; [ScenarioWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Ri - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($file); $g.Dispose(); $bmp.Dispose()
}

Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Start-Process $azahar -ArgumentList "`"$game`""

$seen = 0; $done = $false; $start = Get-Date
$lines = @()
while (-not $done -and ((Get-Date) - $start).TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds 150
    if (-not (Test-Path $log)) { continue }
    $all = @(Get-Content $log -ErrorAction SilentlyContinue)
    for ($i = $seen; $i -lt $all.Count; $i++) {
        $line = $all[$i]; $lines += $line; Write-Host $line
        if ($line -match 'SHOT (\S+)') { Shot (Join-Path $out "$($Matches[1]).png") }
        if ($line -match '\bDONE\b') { $done = $true }
    }
    $seen = $all.Count
}
Start-Sleep -Milliseconds 300
Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $sd 'test.txt') -ErrorAction SilentlyContinue

$pass = @($lines | Where-Object { $_ -match '\] PASS ' }).Count
$fail = @($lines | Where-Object { $_ -match '\] FAIL ' }).Count
$err = @($lines | Where-Object { $_ -match '\] ERROR ' }).Count
$shots = @(Get-ChildItem $out -Filter *.png -ErrorAction SilentlyContinue).Count
if (-not $done) { Write-Host "TIMEOUT after $Timeout s"; $err++ }
Write-Host "== $name : $pass passed, $fail failed, $err errors, $shots screenshots in $out"
if ($fail -gt 0 -or $err -gt 0) { exit 1 }
exit 0
