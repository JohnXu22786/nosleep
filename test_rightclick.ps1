# Kill existing nosleep
taskkill /f /im nosleep.exe 2>$null

# Start nosleep in tray mode with verbose logging
$logFile = "tray_log.txt"
if (Test-Path $logFile) { Remove-Item $logFile }

Write-Host "Starting nosleep.exe (tray mode) with output to $logFile"
$process = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $logFile

Write-Host "Process ID: $($process.Id)"
Write-Host "Waiting 3 seconds for tray icon to appear..."
Start-Sleep -Seconds 3

Write-Host "Please right-click the tray icon (gray Z) and see if menu appears."
Write-Host "After testing, press Enter to kill the process and view logs."
Read-Host "Press Enter"

Stop-Process -Id $process.Id -Force
Write-Host "Process killed. Log content:"
Get-Content $logFile