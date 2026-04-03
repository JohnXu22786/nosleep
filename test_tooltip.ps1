$env:NOSLEEP_DEBUG='1'
$process = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--duration 2 --tray" -NoNewWindow -RedirectStandardOutput "output.log" -RedirectStandardError "error.log" -PassThru
Write-Host "Started nosleep with PID $($process.Id)"
Start-Sleep -Seconds 5
Stop-Process -Id $process.Id -Force
Write-Host "Process stopped. Output:"
Get-Content output.log -ErrorAction SilentlyContinue | Select-Object -First 10
Write-Host "Errors (last 30 lines):"
Get-Content error.log -ErrorAction SilentlyContinue | Select-Object -Last 30
Remove-Item output.log -ErrorAction SilentlyContinue
Remove-Item error.log -ErrorAction SilentlyContinue