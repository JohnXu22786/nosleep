$env:NOSLEEP_DEBUG=1
$process = Start-Process -FilePath "bin/nosleep.exe" -NoNewWindow -PassThru -RedirectStandardOutput "debug_out4.txt" -RedirectStandardError "debug_err4.txt"
Write-Host "Started nosleep with PID $($process.Id)"
Write-Host "Waiting 10 seconds for tray icon to appear..."
Start-Sleep -Seconds 10
Write-Host "Now trying to right-click the tray icon (manually)"
Write-Host "Press Enter to kill process..."
Read-Host
Stop-Process -Id $process.Id -Force
Write-Host "Process killed. Output saved to debug_out4.txt and debug_err4.txt"
Get-Content debug_out4.txt -Tail 30
Write-Host "---"
Get-Content debug_err4.txt -Tail 30