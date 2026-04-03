$log = "out.txt"
if (Test-Path $log) { Remove-Item $log }
$p = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $log
Start-Sleep -Seconds 5
Stop-Process -Id $p.Id -Force
Write-Host "=== Output ==="
Get-Content $log