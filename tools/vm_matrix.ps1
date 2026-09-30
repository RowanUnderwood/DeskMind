# Run TEST.BAT variants in the DeskMind VM several times and report crashes / clean exits.
# Usage: vm_matrix.ps1 -Variants "V640 /GO","V640 /GO /NOSND" [-Runs 3] [-Seconds 85]
param([string[]]$Variants = @("V640 /GO"), [int]$Runs = 3, [int]$Seconds = 85)
$root = "H:\Dos Projects\DOS 286 generative AI applications"
$wslRoot = "/mnt/h/Dos Projects/DOS 286 generative AI applications"
$bat = "$root\vm\files\DMTEST\TEST.BAT"
$orig = [IO.File]::ReadAllText($bat)
try {
  foreach ($v in $Variants) {
    foreach ($i in 1..$Runs) {
      [IO.File]::WriteAllText($bat, "@ECHO OFF`r`nCD \DMTEST`r`n$v`r`n")
      wsl -d Ubuntu-20.04 --cd $wslRoot -- sh tools/build_vm.sh | Out-Null
      $r = & "$root\tools\vmrun.ps1" -Tag "mx$i" -Shots "$Seconds" 2>&1 | Select-String "Cannot find"
      $done = wsl -d Ubuntu-20.04 --cd $wslRoot -- sh -c "MTOOLS_SKIP_CHECK=1 mtype -i vm/dm_test.img@@32256 ::/DMTEST/V640.LOG 2>/dev/null | grep -a -c 'at exit'"
      "{0,-28} run {1}: 86Box crashed={2}  V640 finished={3}" -f $v, $i, [bool]$r, $done
    }
  }
} finally {
  [IO.File]::WriteAllText($bat, $orig)
}
