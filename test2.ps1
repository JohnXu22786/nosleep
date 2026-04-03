$env:NOSLEEP_DEBUG='1'
$p = Start-Process -FilePath "bin/nosleep.exe" -ArgumentList '--tray', '--duration', '1' -PassThru -NoNewWindow -RedirectStandardError "debug2.log"
Start-Sleep -Seconds 5
$p | Stop-Process -Force
Get-Content "debug2.log" -ErrorAction SilentlyContinue