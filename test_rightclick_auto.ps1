$env:NOSLEEP_DEBUG=1
Write-Host "Starting nosleep with version fix..."
Remove-Item debug_out6.txt -ErrorAction SilentlyContinue
Remove-Item debug_err6.txt -ErrorAction SilentlyContinue
$process = Start-Process -FilePath "bin/nosleep.exe" -NoNewWindow -PassThru -RedirectStandardOutput "debug_out6.txt" -RedirectStandardError "debug_err6.txt"
Write-Host "Started nosleep with PID $($process.Id)"
Write-Host "Waiting 10 seconds for tray icon to appear and testing..."
Start-Sleep -Seconds 10
Write-Host "Killing process..."
Stop-Process -Id $process.Id -Force
Start-Sleep -Seconds 1
Write-Host "Process killed. Output saved to debug_out6.txt and debug_err6.txt"
Write-Host "--- Full stdout ---"
Get-Content debug_out6.txt
Write-Host "--- Full stderr ---"
Get-Content debug_err6.txt