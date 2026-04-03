# Kill existing nosleep
taskkill /f /im nosleep.exe 2>$null

# Start nosleep in tray mode with verbose logging
$logFile = "tray_log.txt"
if (Test-Path $logFile) { Remove-Item $logFile }

Write-Host "Starting nosleep.exe (tray mode) with output to $logFile"
# Set debug environment variable to see more logs
$env:NOSLEEP_DEBUG = "1"
$process = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $logFile

Write-Host "Process ID: $($process.Id)"
Write-Host "Tray icon should appear. Please right-click the tray icon (gray Z) within the next 30 seconds."
Write-Host "Mouse over the icon to generate WM_MOUSEMOVE logs."
Write-Host "Waiting 30 seconds..."
Start-Sleep -Seconds 30

Write-Host "Killing process..."
Stop-Process -Id $process.Id -Force
Write-Host "Log content:"
Get-Content $logFile