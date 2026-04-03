$out = "test_out.txt"
$err = "test_err.txt"
if (Test-Path $out) { Remove-Item $out }
if (Test-Path $err) { Remove-Item $err }
$p = Start-Process -FilePath "bin\nosleep.exe" -ArgumentList "--verbose" -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
Start-Sleep -Milliseconds 500
$p | Stop-Process -Force
Write-Host "Stdout:"
Get-Content $out
Write-Host "Stderr:"
Get-Content $err