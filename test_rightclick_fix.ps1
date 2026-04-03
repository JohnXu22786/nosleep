$env:NOSLEEP_DEBUG=1
Write-Host "Starting nosleep with version fix..."
$process = Start-Process -FilePath "bin/nosleep.exe" -NoNewWindow -PassThru -RedirectStandardOutput "debug_out5.txt" -RedirectStandardError "debug_err5.txt"
Write-Host "Started nosleep with PID $($process.Id)"
Write-Host "Waiting 5 seconds for tray icon to appear..."
Start-Sleep -Seconds 5
Write-Host "Now you can right-click the tray icon. After testing, press Enter to kill process."
Read-Host
Stop-Process -Id $process.Id -Force
Write-Host "Process killed. Output saved to debug_out5.txt and debug_err5.txt"
Write-Host "--- Last 30 lines of stdout ---"
Get-Content debug_out5.txt -Tail 30
Write-Host "--- Last 30 lines of stderr ---"
Get-Content debug_err5.txt -Tail 30