$env:NOSLEEP_DEBUG='1'
$process = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--duration", "0", "--prevent-display", "--away-mode", "--verbose" -NoNewWindow -RedirectStandardOutput "output_cli.log" -RedirectStandardError "error_cli.log" -PassThru
Start-Sleep -Seconds 3
Stop-Process -Id $process.Id -Force
Get-Content output_cli.log
Get-Content error_cli.log