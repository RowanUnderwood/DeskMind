# Run the DeskMind 86Box VM off-screen, take screenshots at given seconds, send keys, then kill it.
# Usage: vmrun.ps1 -Tag NAME -Shots "20,40" [-Keys "30=1;35={ENTER}"] [-Vm dm_sl2]
# Uses the parent project's 86Box, ROMs, snap.ps1 and keys.ps1 (read-only).
param([string]$Vm = "dm_sl2", [string]$Tag = "run", [string]$Shots = "30", [string]$Keys = "",
      [string]$OutDir = "H:\Dos Projects\DOS 286 generative AI applications\vm\shots")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WM {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
}
"@
New-Item -ItemType Directory -Force $OutDir | Out-Null
$box = "H:\Dos Projects\86Box"
$tools = "H:\Dos Projects\tools"
$vmDir = "H:\Dos Projects\DOS 286 generative AI applications\vm\$Vm"
$p = Start-Process -FilePath "$box\86Box.exe" -ArgumentList "-P `"$vmDir`" -R `"$box\roms`"" -PassThru
for ($i = 0; $i -lt 50 -and $p.MainWindowHandle -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
$h = $p.MainWindowHandle
[void][WM]::SetWindowPos($h, [IntPtr]::Zero, -3000, 0, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010)
$events = @()
foreach ($s in ($Shots -split ',')) { if ($s) { $events += [pscustomobject]@{ t = [double]$s; k = 'shot'; v = '' } } }
foreach ($kv in ($Keys -split ';')) { if ($kv) { $t, $v = $kv -split '=', 2; $events += [pscustomobject]@{ t = [double]$t; k = 'key'; v = $v } } }
$events = $events | Sort-Object t
$start = Get-Date
foreach ($e in $events) {
  $wait = $e.t - ((Get-Date) - $start).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  if ($e.k -eq 'shot') {
    powershell -NoProfile -File "$PSScriptRoot\snap.ps1" -ProcId $p.Id -Out ("$OutDir\{0}_{1:000}.png" -f $Tag, [int]$e.t)
  } else {
    $prev = [WM]::GetForegroundWindow()
    [WM]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [WM]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [void][WM]::SetForegroundWindow($h); Start-Sleep -Milliseconds 150
    powershell -NoProfile -File "$tools\keys.ps1" -Text $e.v; Start-Sleep -Milliseconds 150
    [WM]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [WM]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [void][WM]::SetForegroundWindow($prev)
  }
}
Stop-Process -Id $p.Id -Force
