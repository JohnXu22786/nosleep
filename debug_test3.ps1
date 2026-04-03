$env:NOSLEEP_DEBUG = "1"
$log = "debug_out3.txt"
$errlog = "debug_err3.txt"
if (Test-Path $log) { Remove-Item $log }
if (Test-Path $errlog) { Remove-Item $errlog }
Write-Host "Starting nosleep in tray mode (no args)..."
$p = Start-Process -FilePath "bin\nosleep.exe" -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError $errlog
Start-Sleep -Seconds 5
Write-Host "Stopping process..."
Stop-Process -Id $p.Id -Force
Write-Host "=== Stdout ==="
Get-Content $log
Write-Host "=== Stderr ==="
Get-Content $errlog