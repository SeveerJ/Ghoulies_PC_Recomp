# ghoulies_watch.ps1 -- lets Claude start Ghoulies runs by itself.
# Separate from Orta's watcher: it only looks in the ghoulies folder.
#
# Start once from Developer PowerShell for VS 2022 and leave the window open:
#   powershell -ExecutionPolicy Bypass -File G:\xboxrecomp\ghoulies_recomp\ghoulies\ghoulies_watch.ps1
#
# The first time, it runs ghoulies_setup.ps1 (copy toolkit, extract disc).
# After that it waits for go.flag, runs ghoulies_loop.ps1, saves the output to
# loop_output.txt and writes watch_status.txt.

$game   = 'G:\xboxrecomp\ghoulies_recomp\ghoulies'
$go     = Join-Path $game 'go.flag'
$status = Join-Path $game 'watch_status.txt'
$out    = Join-Path $game 'loop_output.txt'

Set-Location $game
if (-not (Test-Path 'G:\xboxrecomp\ghoulies_recomp\xboxrecomp-main\game_files\default.xbe')) {
    "setup running $(Get-Date -Format o)" | Set-Content $status
    Write-Host 'First start: copying the toolkit and extracting the disc. This takes a while...' -ForegroundColor Yellow
    powershell -ExecutionPolicy Bypass -File (Join-Path $game 'ghoulies_setup.ps1') *> (Join-Path $game 'setup_output.txt')
    "setup finished $(Get-Date -Format o) exit $LASTEXITCODE" | Set-Content $status
    Write-Host 'Setup finished.' -ForegroundColor Green
} else {
    "idle $(Get-Date -Format o)" | Set-Content $status
}
Write-Host 'Watching for Ghoulies runs requested by Claude. Leave this window open.' -ForegroundColor Cyan

while ($true) {
    if (Test-Path $go) {
        Remove-Item $go -ErrorAction SilentlyContinue
        "running $(Get-Date -Format o)" | Set-Content $status
        Write-Host "`n$(Get-Date -Format T)  Claude started a run..." -ForegroundColor Yellow
        powershell -ExecutionPolicy Bypass -File (Join-Path $game 'ghoulies_loop.ps1') *> $out
        $code = $LASTEXITCODE
        "finished $(Get-Date -Format o) exit $code" | Set-Content $status
        Write-Host "$(Get-Date -Format T)  Run finished. Waiting for the next one." -ForegroundColor Green
    }
    Start-Sleep -Seconds 5
}
