# Kill existing
taskkill /f /im nosleep.exe 2>$null

$log = "interactive_log.txt"
if (Test-Path $log) { Remove-Item $log }

Write-Host "Starting nosleep in tray mode..."
# Set debug environment variable to see mouse events
$env:NOSLEEP_DEBUG = "1"
$process = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $log
Write-Host "Process started with PID $($process.Id)"
Write-Host "Tray icon should appear. Please right-click the tray icon (gray Z) and check if context menu appears."
Write-Host "Also hover mouse over the icon to generate WM_MOUSEMOVE logs."
Write-Host "After testing, press Enter to stop the process and view logs."
Read-Host "Press Enter" | Out-Null

Write-Host "Stopping process..."
Stop-Process -Id $process.Id -Force
Write-Host "=== LOG CONTENT ==="
Get-Content $log