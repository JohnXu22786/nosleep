$out = "tray_start_out.txt"
$err = "tray_start_err.txt"
if (Test-Path $out) { Remove-Item $out }
if (Test-Path $err) { Remove-Item $err }
$p = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--tray --verbose" -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
Start-Sleep -Seconds 1
$p | Stop-Process -Force
Write-Host "=== STDOUT ==="
Get-Content $out
Write-Host "=== STDERR ==="
Get-Content $err