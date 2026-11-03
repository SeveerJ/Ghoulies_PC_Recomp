# ghoulies_loop.ps1 -- one round of the Grabbed by the Ghoulies loop.
# Same steps as Orta's loop, but every path is inside G:\xboxrecomp\ghoulies_recomp:
#   1. collect missing function addresses from the last run's log
#   2. if there are new ones, re-run the pipeline (disasm -> func_id -> abi_analysis -> recomp)
#   3. rebuild the .exe (configures build64 the first time)
#   4. run it (unless build_only.flag contains 1)
#   5. print a short summary

$toolkit  = 'G:\xboxrecomp\ghoulies_recomp\xboxrecomp-main'
$game     = 'G:\xboxrecomp\ghoulies_recomp\ghoulies'
$seeds    = Join-Path $toolkit 'game_files\seeds_manual.json'
$log      = Join-Path $game 'stderr.txt'
# Non-.text code sections of the Ghoulies XBE (statically linked libraries).
$sections = 'D3D,D3DX,XGRPH,BINK,BINK32,BINK32A,BINK16,BINK4444,BINK5551,BINKYUY2,BINK16MX,BINK16X2,BINK16M,BINK32MX,BINK32X2,BINK32M,XACTENG,DSOUND,WMADEC,XPP'
# Guest code range: addresses outside it are not functions.
$codeLo = 0x11000
$codeHi = 0x202EA0   # end of XPP (start of .rdata)

# Bring-up: the watchdog ends a run after this many seconds and dumps the
# guest stack; a run that stops drawing for $stallSecs also ends itself.
# Kept short while the game has not drawn a frame: everything a hung boot
# tells us is in the log within ~30 s. Lengthen once it reaches the title.
# (The first run after a shader change compiles the GPU shaders once, which can
# take minutes; later runs load them from build64\Release\shadercache.)
$watchdogSeconds = 7200
$runLimitSeconds = 7500
$stallSecs       = 60
$kernelLogBudget = 400

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Fail($what) {
    Write-Host "`n$what failed. Send Claude the lines above." -ForegroundColor Red
    Pop-Location -ErrorAction SilentlyContinue
    exit 1
}
function RunTool($name, [scriptblock]$cmd) {
    Step $name
    $out = & $cmd 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $out | Select-Object -Last 25 | ForEach-Object { "$_" }
        Fail $name
    }
    $out | Select-Object -Last 2 | ForEach-Object { "$_" }
}

# ---- 1. Collect missing functions ------------------------------------------
Step 'Collecting missing functions from the last run'
$known = @()
if (Test-Path $seeds) {
    $known = @((Get-Content $seeds -Raw | ConvertFrom-Json) | ForEach-Object { $_.start })
}
$found = @()
if (Test-Path $log) {
    $found = @(Select-String -Path $log -Pattern 'Failed to resolve VA (0x[0-9A-Fa-f]+)' |
        ForEach-Object { '0x' + $_.Matches[0].Groups[1].Value.Substring(2).ToUpper() } |
        Where-Object { $v = [Convert]::ToUInt32($_, 16); $v -ge $codeLo -and $v -lt $codeHi })
    $found += @(Select-String -Path $log -Pattern '\[STUB\] code at 0x([0-9A-Fa-f]{8}) skipped' |
        ForEach-Object { '0x' + $_.Matches[0].Groups[1].Value.ToUpper() } |
        Where-Object { $v = [Convert]::ToUInt32($_, 16); $v -ge $codeLo -and $v -lt $codeHi })
}
$new = @($found | Sort-Object -Unique | Where-Object { $known -notcontains $_ })
$all = @(($known + $new) | Sort-Object -Unique)
Write-Host "Already known: $($known.Count)   New from last run: $($new.Count)   Total: $($all.Count)"

$aliasSeeds = Join-Path $toolkit 'game_files\alias_seeds.json'
if (-not (Test-Path $aliasSeeds)) { Set-Content -Path $aliasSeeds -Value '[]' -Encoding ASCII }
$env:XBOXRECOMP_ALIAS_SEEDS = $aliasSeeds

$genStamp = Join-Path $game 'last_translation.stamp'
$translatorChanged = -not (Test-Path $genStamp)
if (Test-Path $genStamp) {
    $genTime = (Get-Item $genStamp).LastWriteTime
    foreach ($f in 'tools\disasm\functions.py', 'game_files\alias_seeds.json', 'tools\recomp\lifter.py', 'tools\recomp\translator.py', 'game_files\trace_functions.json', 'game_files\seeds_manual.json', '..\ghoulies\src\recomp_manual.c') {
        if (-not (Test-Path (Join-Path $toolkit $f))) { continue }
        if ((Get-Item (Join-Path $toolkit $f)).LastWriteTime -gt $genTime) { $translatorChanged = $true }
    }
}
if ($translatorChanged) { Write-Host 'Translation needed (first run, or the translator / manual code changed).' }

if ($new.Count -gt 0 -or $translatorChanged) {
    if ($new.Count -gt 0) { Write-Host ("New: " + ($new -join ', ')) }
    $json = '[' + (($all | ForEach-Object { '{"start": "' + $_ + '", "observed": true}' }) -join ', ') + ']'
    Set-Content -Path $seeds -Value $json -Encoding ASCII

    # ---- 2. Pipeline -------------------------------------------------------
    Push-Location $toolkit
    if (-not (Test-Path 'game_files/default_analysis.json')) {
        RunTool 'Reading the XBE (first time only)' { py -3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json }
    }
    if ($sections) {
        RunTool 'Disassembling' { py -3 -m tools.disasm game_files/default.xbe --text-only --extra-sections $sections --seed-functions game_files/seeds_manual.json --force }
    } else {
        RunTool 'Disassembling' { py -3 -m tools.disasm game_files/default.xbe --text-only --seed-functions game_files/seeds_manual.json --force }
    }
    RunTool 'Labelling functions' { py -3 -m tools.func_id game_files/default.xbe }
    RunTool 'Checking calling conventions' { py -3 -m tools.abi_analysis game_files/default.xbe }
    RunTool 'Translating to C' {
        if (Test-Path 'game_files/trace_functions.json') {
            py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../ghoulies/src/recomp/gen --exclude-manual ../ghoulies/src/recomp_manual.c --trace-functions game_files/trace_functions.json
        } else {
            py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../ghoulies/src/recomp/gen --exclude-manual ../ghoulies/src/recomp_manual.c
        }
    }
    Set-Content -Path $genStamp -Value (Get-Date -Format o)
    Pop-Location
} else {
    Write-Host 'Nothing new to translate, going straight to build.'
}

# ---- 3. Build ----------------------------------------------------------------
Push-Location $game
Get-Process ghoulies_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not (Test-Path 'build64\CMakeCache.txt')) {
    Step 'Configuring the build (first time only)'
    $cfg = cmake -S . -B build64 -A x64 2>&1
    if ($LASTEXITCODE -ne 0) { $cfg | Select-Object -Last 30 | ForEach-Object { "$_" }; Fail 'Configure' }
}
Step 'Building the .exe'
$buildOut = cmake --build build64 --config Release 2>&1
if ($LASTEXITCODE -ne 0) {
    $buildOut | Select-String ' error ' | Select-Object -First 40 | ForEach-Object { $_.Line }
    Fail 'Build'
}
Write-Host 'Build OK'
$buildOnly = Join-Path $game 'build_only.flag'
if ((Test-Path $buildOnly) -and ((Get-Content $buildOnly -Raw).Trim() -eq '1')) {
    Write-Host 'Build only: the game was not started.'
    Pop-Location
    exit 0
}

# ---- 4. Run ------------------------------------------------------------------
Step "Running the game (up to $watchdogSeconds s)"
$env:RECOMP_WATCHDOG_SECS     = "$watchdogSeconds"
$env:RECOMP_KERNEL_LOG_BUDGET = "$kernelLogBudget"
$env:RECOMP_STALL_EXIT_SECS   = "$stallSecs"
$env:RECOMP_PB_EXEC           = '1'
$env:RECOMP_GPU               = '1'
$env:RECOMP_GPU_VSH           = '1'
$env:RECOMP_PROFILE           = '1'
$env:RECOMP_FAST_FLUSH        = '1'
# PUT settle off again: the kick-off now waits for the reader (sub_00177280
# in recomp_manual.c), so D3D never writes over commands not yet read.
$env:RECOMP_PUT_SETTLE_US     = '0'
$env:RECOMP_TITLE_KEVENTS     = '1'
$env:RECOMP_PCRTC_TRAP        = '1'
$env:RECOMP_VBLANK            = '1'
$env:RECOMP_VBLANK_LEGACY     = '0'
$env:RECOMP_TIMER_RES         = '0'
$env:RECOMP_HEAP_RECLAIM      = '1'
$env:RECOMP_AC97_READY        = '1'
$env:RECOMP_APU_PHYS_CONTIG_FIRST = '1'
$env:RECOMP_TRACE_ARGS        = '5'
$env:RECOMP_GPU_SCALE         = '2'
# Ghoulies' D3D (XDK 5659): the device pointer global, where the device keeps
# its pointer to the GPU-progress semaphore (+0x30; Orta's 4928 used +0x34),
# and its GPU-busy event (+0x1DBC; Orta's was +0x2564).
$env:RECOMP_SEMA_DEVPTR       = '0x181E08'
$env:RECOMP_SEMA_DEVOFF       = '0x30'
$env:RECOMP_GPU_INTR_EVENT    = '0x1DBC'
# AV pack answer: standard AV cable, NTSC-M, 60 Hz, widescreen allowed
# (0x00010000). The game asks for widescreen 640x480 (present flags 0x110);
# this D3D (0x17B85D) refuses a widescreen mode unless that bit is set, and
# CreateDevice failed with E_FAIL, leaving the game on a null device.
$env:RECOMP_AV_PACK_INFO      = '0x00410101'
# Mirror each GPU fence release into PGRAPH 0x400B10: this D3D's wait-for-idle
# (0x1771A0) compares that register with the fence word in memory.
$env:RECOMP_SEMA_MIRROR       = '0x400B10'
# Flips: hand each "flip here" request in the command stream to D3D's own
# software-method handler (0x17C000) at the next vblank, so swaps are counted
# and the vblank callback reports "swap done". The game's frame clock is
# vblanks-per-swap; with no swaps it read 0 and nothing ever moved.
$env:RECOMP_FLIP_SWMETHOD     = '0x17C000'
# The game may run up to this far ahead of the GPU (bytes of commands): the
# two overlap instead of taking turns. '0' = the old full wait.
# Back to 0: at 524288 a room load hung -- D3D queued fences without kicking
# them and then waited on them with the reader idle at PUT.
$env:RECOMP_SYNC_KICK_LAG      = '0'
# One processor's worth of DISPATCH_LEVEL: a thread raised to it keeps the
# others (the audio DPC above all) out until it lowers again, as on the Xbox.
# A spider hit into a wall crashed DirectSound without it. '0' turns it off.
$env:RECOMP_IRQL_LOCK          = '1'
# Grunts play too high: log each voice's format and pitch as it starts.
$env:RECOMP_APU_VOICELOG      = '0'
# Driver threading stays on: the crashes inside nvwgf2umx.dll were ours (a
# texture view bound after it was freed), not the driver's threads.
$env:RECOMP_GPU_NO_DRV_THREADS = '0'
# Every 600 frames: which GPU surfaces exist and which one the flip shows
# (the window was black while guest memory held the picture).
$env:RECOMP_SURF_CENSUS       = '1'
# Shader optimisation level 3: the two big base shaders are already in the
# cache at this level (level 0 meant converting them again, ~2 min each).
$env:RECOMP_SHADER_OPT        = '3'
# Follow command-buffer jumps outside D3D's ring: the game jumps to recorded
# command buffers (0x81010100) and back. Refusing them dropped commands up to
# PUT, fences included, and the game froze waiting for them.
$env:RECOMP_PB_FOREIGN_JUMPS  = '0'   # the 'jump' was stale data, not a real jump
# Ghoulies runs at 30 fps on the Xbox (it asks for a flip every second
# vblank); at 60 it played at double speed.
# 60 fps: the game's own frame timing follows the flips, so asking each flip
# for every vblank (interval 1) runs it at 60 at the right speed. Set
# $fps60 = $false to go back to the original 30.
$fps60 = $true
if ($fps60) {
    $env:RECOMP_FLIP_INTERVAL = '1'
    $env:RECOMP_FPS_CAP       = '60'
} else {
    Remove-Item Env:RECOMP_FLIP_INTERVAL -ErrorAction SilentlyContinue
    $env:RECOMP_FPS_CAP       = '30'
}
# Input recording / replay (port 1). Every run records to inputs_last.txt;
# if inputs_replay.txt exists it is played back first, so a test run can
# reach the same place without anyone at the pad. Touching the pad ends it.
$env:RECOMP_INPUT_RECORD      = (Join-Path $game 'inputs_last.txt')
# Replay is off unless asked for (the user wants it only for launch tests):
# set $useReplay = $true to play inputs_replay.txt back.
$useReplay = $false
if ($useReplay -and (Test-Path (Join-Path $game 'inputs_replay.txt'))) {
    $env:RECOMP_INPUT_REPLAY  = (Join-Path $game 'inputs_replay.txt')
    $env:RECOMP_INPUT_REPLAY_STRETCH = '110'   # replay timings already come from a replayed run
} else {
    Remove-Item Env:RECOMP_INPUT_REPLAY -ErrorAction SilentlyContinue
}
# The first time the command reader finds a bad spot, save the ring.
$env:RECOMP_PB_RING_DUMP      = (Join-Path $game 'ringdump.bin')
# Black-screen diagnosis: trace one whole frame (every draw, its state and the
# pixels it wrote) when ftrace.flag exists, and save GPU-side screenshots.
$env:RECOMP_FRAME_TRACE       = (Join-Path $game 'ftrace3.flag')
$env:RECOMP_FRAME_TRACE_METHODS = '1'   # every GPU command in the traced frame
$env:RECOMP_GPU_SHOT          = (Join-Path $game 'gpushot')
# Save each texture the game uses (first use) to texdump\ to check how they decode.
$texdump = Join-Path $game 'texdump'
New-Item -ItemType Directory -Force -Path $texdump | Out-Null
Get-ChildItem -Path $texdump -Filter '*.bmp' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
$env:RECOMP_TEX_DUMP          = (Join-Path $texdump 't')
Get-ChildItem -Path $game -Filter 'gpushot*.bmp' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
$env:RECOMP_FB_WINDOW         = '1'
$env:RECOMP_FB_DUMP           = (Join-Path $game 'frame.bmp')
$env:RECOMP_FB_WINDOW_DUMP_EVERY = '300'
# Play mode: none of the periodic dumps (GPU screenshots every 600 frames,
# every new texture saved as a .bmp, frame.bmp every 300 frames, the surface
# census) -- each one is a stall. F11 captures and crash reports still work.
# Set $playMode = $false to bring the dumps back for debugging.
$playMode = $true
if ($playMode) {
    foreach ($v in 'RECOMP_GPU_SHOT', 'RECOMP_TEX_DUMP', 'RECOMP_SURF_CENSUS',
                   'RECOMP_FB_WINDOW_DUMP_EVERY', 'RECOMP_FRAME_TRACE',
                   'RECOMP_FRAME_TRACE_METHODS') {
        Remove-Item "Env:$v" -ErrorAction SilentlyContinue
    }
}
$timer = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $game 'build64\Release\ghoulies_recomp.exe') `
    -WorkingDirectory $game -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $game 'stdout.txt') `
    -RedirectStandardError $log
$handle = $p.Handle
if (-not $p.WaitForExit($runLimitSeconds * 1000)) {
    Write-Host 'Still running after the safety limit, stopping it.' -ForegroundColor Yellow
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit()
}
$timer.Stop()
$secs = [int]$timer.Elapsed.TotalSeconds
$code = $p.ExitCode
Pop-Location

# ---- 5. Summary --------------------------------------------------------------
Step 'Summary of this run'
$how = switch ($code) { 0 { 'the game exited normally' } 3 { 'the watchdog ended it' } default { 'it crashed or was stopped' } }
Write-Host ("Ran for {0} s, exit code {1}: {2}" -f $secs, $code, $how)
if (Test-Path $log) {
    $lines = Get-Content $log -Encoding UTF8 -Tail 4000
    $misses = @(Select-String -Path $log -Pattern 'Failed to resolve VA (0x[0-9A-Fa-f]+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
    Write-Host "Missing functions this run: $($misses.Count)"
    Write-Host "`n--- Important lines ---"
    $lines | Select-String 'ICALL\] Failed|is not code|called from|CRASH|WATCHDOG|Xbox VA of fault|  in sub_|  regs:|REGION|PAD|STUB|UNIMPL| fps \(' |
        Select-Object -Last 40 | ForEach-Object { $_.Line }
    Write-Host "`n--- Last 30 log lines (stack dump hidden) ---"
    $lines | Where-Object { $_ -notmatch '^\s+GS ' } | Select-Object -Last 30
} else {
    Write-Host 'No log found.'
}
