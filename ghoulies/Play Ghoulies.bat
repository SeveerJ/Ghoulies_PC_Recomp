@echo off
rem Play Grabbed by the Ghoulies (PC port) -- just play: no rebuild, no test extras.
rem Starts the last build with the same settings the test runs use.
rem If it crashes, play_log.txt next to this file says why (send it to Claude).
cd /d "%~dp0"
if not exist "build64\Release\ghoulies_recomp.exe" (
  echo The game has not been built yet - ask Claude for a build first.
  pause
  exit /b 1
)

rem --- what the port needs to run ---
set RECOMP_PB_EXEC=1
set RECOMP_GPU=1
set RECOMP_GPU_VSH=1
set RECOMP_FB_WINDOW=1
set RECOMP_FAST_FLUSH=1
set RECOMP_PUT_SETTLE_US=0
set RECOMP_TITLE_KEVENTS=1
set RECOMP_PCRTC_TRAP=1
set RECOMP_VBLANK=1
set RECOMP_VBLANK_LEGACY=0
set RECOMP_TIMER_RES=0
set RECOMP_HEAP_RECLAIM=1
set RECOMP_AC97_READY=1
set RECOMP_APU_PHYS_CONTIG_FIRST=1
set RECOMP_TRACE_ARGS=5
set RECOMP_KERNEL_LOG_BUDGET=400
set RECOMP_SEMA_DEVPTR=0x181E08
set RECOMP_SEMA_DEVOFF=0x30
set RECOMP_GPU_INTR_EVENT=0x1DBC
set RECOMP_AV_PACK_INFO=0x00410101
set RECOMP_SEMA_MIRROR=0x400B10
set RECOMP_FLIP_SWMETHOD=0x17C000
set RECOMP_SYNC_KICK_LAG=0
set RECOMP_IRQL_LOCK=1
set RECOMP_GPU_NO_DRV_THREADS=0
set RECOMP_SHADER_OPT=3
set RECOMP_PB_FOREIGN_JUMPS=0

rem --- picture: 2 = drawn at twice the Xbox's resolution ---
set RECOMP_GPU_SCALE=2

rem --- frame rate: 60 fps. For the original 30, change these two lines to
rem     set RECOMP_FLIP_INTERVAL=
rem     set RECOMP_FPS_CAP=30
set RECOMP_FLIP_INTERVAL=1
set RECOMP_FPS_CAP=60

rem --- F11 still saves a frame capture for bug reports ---
set RECOMP_FB_DUMP=%~dp0frame.bmp

start "" /b /wait "build64\Release\ghoulies_recomp.exe" 1>play_stdout.txt 2>play_log.txt
