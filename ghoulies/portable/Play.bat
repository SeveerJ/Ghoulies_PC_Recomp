@echo off
rem Starts the game and keeps a log - if it crashes, play_log.txt says why.
cd /d "%~dp0"
"%~dp0ghoulies_recomp.exe" 1>"%~dp0play_stdout.txt" 2>"%~dp0play_log.txt"
