# ghoulies_setup.ps1 -- one-time setup for the Grabbed by the Ghoulies port.
# Run automatically by ghoulies_watch.ps1 the first time it starts.
#
# It only READS from the Orta folders (G:\xboxrecomp\xboxrecomp-main\...).
# Everything it writes goes under G:\xboxrecomp\ghoulies_recomp.
#   1. copies the toolkit to ghoulies_recomp\xboxrecomp-main, leaving out
#      Orta's game files and Orta's disassembly / analysis output
#   2. copies Dear ImGui (for the F1 menu) from orta\third_party
#   3. extracts the Ghoulies disc into ghoulies_recomp\xboxrecomp-main\game_files

$root       = 'G:\xboxrecomp\ghoulies_recomp'
$srcToolkit = 'G:\xboxrecomp\xboxrecomp-main\xboxrecomp-main'
$srcOrta    = 'G:\xboxrecomp\xboxrecomp-main\orta'
$toolkit    = Join-Path $root 'xboxrecomp-main'
$game       = Join-Path $root 'ghoulies'
$iso        = 'G:\xboxrecomp\Grabbed by the Ghoulies (USA) (En,Fr,De,Es,It)\Grabbed by the Ghoulies (USA) (En,Fr,De,Es,It).iso'
$report     = Join-Path $game 'setup_report.txt'

"started $(Get-Date -Format o)" | Set-Content $report
function Note($s) { Write-Host $s; $s | Add-Content $report }

# ---- 1. Toolkit copy (no Orta game data) ------------------------------------
Note '=== Copying the toolkit (Orta game files and outputs left out) ==='
$skip = @(
    (Join-Path $srcToolkit 'game_files'),
    (Join-Path $srcToolkit 'tools\disasm\output'),
    (Join-Path $srcToolkit 'tools\func_id\output'),
    (Join-Path $srcToolkit 'tools\abi_analysis\output'),
    (Join-Path $srcToolkit 'tools\recomp\output'),
    '__pycache__', '.git', 'build', 'build64'
)
robocopy $srcToolkit $toolkit /E /XD @skip /NFL /NDL /NJH /NP | Out-Null
$rc = $LASTEXITCODE
Note "toolkit robocopy exit $rc (0-7 = OK)"
if ($rc -ge 8) { Note 'TOOLKIT COPY FAILED'; exit 1 }
foreach ($d in 'game_files', 'tools\disasm\output', 'tools\func_id\output', 'tools\abi_analysis\output', 'tools\recomp\output') {
    New-Item -ItemType Directory -Force -Path (Join-Path $toolkit $d) | Out-Null
}

# ---- 2. Dear ImGui ----------------------------------------------------------
Note '=== Copying Dear ImGui ==='
robocopy (Join-Path $srcOrta 'third_party') (Join-Path $game 'third_party') /E /NFL /NDL /NJH /NP | Out-Null
Note "imgui robocopy exit $LASTEXITCODE"

# ---- 3. Extract the disc ----------------------------------------------------
$gf = Join-Path $toolkit 'game_files'
if (Test-Path (Join-Path $gf 'default.xbe')) {
    Note 'Disc already extracted, skipping.'
} else {
    Note '=== Extracting the Ghoulies disc (several minutes) ==='
    $x = & (Join-Path $toolkit 'xdvdfs.exe') unpack $iso $gf 2>&1
    Note "xdvdfs exit $LASTEXITCODE"
    $x | Select-Object -Last 10 | ForEach-Object { Note "  $_" }
}
if (Test-Path (Join-Path $gf 'default.xbe')) {
    $files = Get-ChildItem $gf -Recurse -File
    Note ('OK: default.xbe present; {0} files, {1:N2} GB' -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1GB))
    Note '--- top level of game_files ---'
    Get-ChildItem $gf | ForEach-Object { Note ('  {0}{1}  {2}' -f $_.Name, $(if ($_.PSIsContainer) { '\' } else { '' }), $(if ($_.PSIsContainer) { '' } else { $_.Length })) }
} else {
    Note 'EXTRACT FAILED: no default.xbe'
    exit 1
}
"finished $(Get-Date -Format o)" | Add-Content $report
exit 0
