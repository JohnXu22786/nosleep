$env:NOSLEEP_DEBUG = "1"
$log = "debug_out.txt"
if (Test-Path $log) { Remove-Item $log }
Write-Host "Starting nosleep with debug..."
$p = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $log
Start-Sleep -Seconds 5
Write-Host "Stopping process..."
Stop-Process -Id $p.Id -Force
Write-Host "=== Output ==="
Get-Content $log