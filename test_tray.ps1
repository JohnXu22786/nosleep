$env:NOSLEEP_DEBUG='1'
$process = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--tray" -NoNewWindow -RedirectStandardOutput "output.log" -RedirectStandardError "error.log" -PassThru
Start-Sleep -Seconds 5
Stop-Process -Id $process.Id -Force
Get-Content output.log
Get-Content error.log