# Put DeskMind files into C:\<Folder> on the Tandy's hard-disk image (SD card), following the
# parent project's rules: fresh dated backup from the card, write a work copy with mtools (WSL),
# check it (fsck.fat -n, 7z t, listing diff), copy it back once, cmp, then a dated "after" backup.
#
# Usage: card_install.ps1 -Tag deskmind-tests [-Folder DMTEST] [-Files a.exe,b.raw] [-Also PLAY]
#   -Files  src[=NAME] goes to C:\<Folder>\NAME; src=DIR/NAME goes to C:\DIR\NAME (DIR must be in -Also)
#   -Also   other root folders this install may change (e.g. PLAY for the launchers)
param([Parameter(Mandatory)][string]$Tag, [string]$Folder = "DMTEST", [string[]]$Files = @(), [string[]]$Also = @())
$ErrorActionPreference = "Stop"
$root   = "H:\Dos Projects\DOS 286 generative AI applications"
$card   = "D:\HDD\D62_500M.img"
$bdir   = "H:\Dos Projects\backup"
$work   = "$bdir\work_deskmind.img"
$date   = Get-Date -Format "yyyy-MM-dd"
$z      = "C:\Program Files\7-Zip\7z.exe"
function WslPath($p) { "/mnt/" + $p.Substring(0,1).ToLower() + ($p.Substring(2) -replace '\\','/') }

if (-not (Test-Path $card)) { throw "The SD card image $card is not there. Put the card in the PC first." }
if (-not $Files) {
  $Files = @("$root\dos\out\V640.EXE", "$root\dos\out\NETTEST.EXE",
             "$root\helper\out\t640.raw=TEST640.RAW", "$root\helper\out\t320.raw=TEST320.RAW")
}

$before = "$bdir\D62_500M_${date}_before-$Tag.img"
$after  = "$bdir\D62_500M_${date}_after-$Tag.img"
"$(Get-Date -Format HH:mm:ss) 1. Backup from the card -> $before"
Copy-Item $card $before
"$(Get-Date -Format HH:mm:ss) 2. Work copy"
Copy-Item $card $work

"$(Get-Date -Format HH:mm:ss) 3. Copy files into C:\$Folder (mtools)"
$wimg = (WslPath $work) + "@@32256"
# mtools asks questions on name clashes; </dev/null makes them fail at once instead of waiting forever
wsl -d Ubuntu-20.04 -- sh -c "export MTOOLS_SKIP_CHECK=1; mdir -i '$wimg' ::/$Folder >/dev/null 2>&1 || mmd -i '$wimg' ::/$Folder </dev/null"
if ($LASTEXITCODE) { throw "Could not create C:\$Folder in the work copy" }
foreach ($f in $Files) {
  $src, $name = $f -split '=', 2
  if (-not $name) { $name = [IO.Path]::GetFileName($src).ToUpper() }
  $dest = "::/$Folder/$name"
  if ($name -match '/') {
    if ($Also -notcontains ($name -split '/')[0]) { throw "$name is outside $Folder and not in -Also" }
    $dest = "::/$name"
  }
  wsl -d Ubuntu-20.04 -- sh -c "MTOOLS_SKIP_CHECK=1 mcopy -o -i '$wimg' '$(WslPath $src)' $dest </dev/null"
  if ($LASTEXITCODE) { throw "mcopy failed for $src" }
}
wsl -d Ubuntu-20.04 -- sh -c "MTOOLS_SKIP_CHECK=1 mdir -i '$wimg' ::/$Folder </dev/null"

"$(Get-Date -Format HH:mm:ss) 4. Checks"
$fsck = wsl -d Ubuntu-20.04 -- sh -c "dd if='$(WslPath $work)' of=/tmp/dm_part.img bs=512 skip=63 status=none && fsck.fat -n /tmp/dm_part.img; echo rc=`$?; rm -f /tmp/dm_part.img"
$fsck | Select-Object -Last 3
if (-not ($fsck -match "rc=0")) { throw "fsck.fat reported problems; the card was NOT changed" }
$t = & $z t $work 2>&1 | Select-String "Everything is Ok|ERROR"
"7z t: $t"
if (-not ($t -match "Everything is Ok")) { throw "7z test failed; the card was NOT changed" }
$la = & $z l -slt $before | Select-String "^Path = " | ForEach-Object { $_.Line } | Where-Object { $_ -notmatch '\.img$' }
$lb = & $z l -slt $work   | Select-String "^Path = " | ForEach-Object { $_.Line } | Where-Object { $_ -notmatch '\.img$' }
$diff = Compare-Object $la $lb
$ok = "(" + ((@($Folder) + $Also) -join "|") + ")(\\|$)"     # folders this install may change
"Listing changes (should only be C:\$Folder $Also):"
$diff | ForEach-Object { "  {0} {1}" -f $_.SideIndicator, $_.InputObject }
if ($diff | Where-Object { $_.SideIndicator -eq "<=" }) { throw "Files disappeared from the image; the card was NOT changed" }
if ($diff | Where-Object { $_.InputObject -notmatch "^Path = $ok" }) { throw "Unexpected changes outside $Folder $Also; the card was NOT changed" }

foreach ($d83 in @($Folder) + $Also) {
  python "$root\tools\check_83.py" $work $d83
  if ($LASTEXITCODE) { throw "Long-name entries found; the card was NOT changed" }
}
"Full extract of backup and work copy, diff -rq (only $Folder may differ):"
$xb = "$env:TEMP\dm_x_before"; $xa = "$env:TEMP\dm_x_after"
Remove-Item $xb, $xa -Recurse -Force -ErrorAction SilentlyContinue
& $z x -y "-o$xb" $before | Out-Null; & $z x -y "-o$xa" $work | Out-Null
$d = python "$root\tools\tree_diff.py" $xb $xa
$d | ForEach-Object { "  $_" }
Remove-Item $xb, $xa -Recurse -Force -ErrorAction SilentlyContinue
if ($d | Where-Object { $_ -notmatch "^(Only in after|Files differ): $ok" }) { throw "Unexpected differences outside $Folder $Also; the card was NOT changed" }

"$(Get-Date -Format HH:mm:ss) 5. Copy back to the card and compare"
Copy-Item $work $card -Force
cmd /c "fc /b `"$work`" `"$card`" >nul" ; if ($LASTEXITCODE) { throw "Card copy differs from the work image!" }
"   card matches the work image"
"$(Get-Date -Format HH:mm:ss) 6. After-backup -> $after"
Move-Item $work $after -Force
"Done."
