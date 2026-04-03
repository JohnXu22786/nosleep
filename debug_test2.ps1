$env:NOSLEEP_DEBUG = "1"
$log = "debug_out2.txt"
$errlog = "debug_err2.txt"
if (Test-Path $log) { Remove-Item $log }
if (Test-Path $errlog) { Remove-Item $errlog }
Write-Host "Starting nosleep with debug..."
$p = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError $errlog
Start-Sleep -Seconds 2
Write-Host "Stopping process..."
Stop-Process -Id $p.Id -Force
Write-Host "=== Stdout ==="
Get-Content $log
Write-Host "=== Stderr ==="
Get-Content $errlog