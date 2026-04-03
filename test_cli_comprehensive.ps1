Write-Host "Testing CLI mode comprehensively..." -ForegroundColor Cyan

# Test 1: Simple indefinite run (should start and be stoppable by Ctrl+C)
Write-Host "`nTest 1: Simple indefinite run (10 seconds)" -ForegroundColor Yellow
$process1 = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--duration", "0", "--verbose" -NoNewWindow -PassThru
Start-Sleep -Seconds 10
Stop-Process -Id $process1.Id -Force
Write-Host "Test 1 completed"

# Test 2: Duration 1 minute (short test)
Write-Host "`nTest 2: 1 minute duration (run for 5 seconds)" -ForegroundColor Yellow
$process2 = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--duration", "1", "--verbose" -NoNewWindow -RedirectStandardOutput "cli_test_1min_out.log" -RedirectStandardError "cli_test_1min_err.log" -PassThru
Start-Sleep -Seconds 5
Stop-Process -Id $process2.Id -Force
if (Test-Path "cli_test_1min_out.log") {
    Write-Host "Output:" -ForegroundColor Green
    Get-Content "cli_test_1min_out.log"
}
if (Test-Path "cli_test_1min_err.log") {
    Write-Host "Errors:" -ForegroundColor Red
    Get-Content "cli_test_1min_err.log"
}

# Test 3: With prevent-display and away-mode
Write-Host "`nTest 3: With prevent-display and away-mode flags" -ForegroundColor Yellow
$process3 = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--duration", "0", "--prevent-display", "--away-mode", "--verbose" -NoNewWindow -RedirectStandardOutput "cli_test_flags_out.log" -RedirectStandardError "cli_test_flags_err.log" -PassThru
Start-Sleep -Seconds 3
Stop-Process -Id $process3.Id -Force
if (Test-Path "cli_test_flags_out.log") {
    Write-Host "Output:" -ForegroundColor Green
    Get-Content "cli_test_flags_out.log" | Select-Object -First 10
}

# Test 4: Custom interval
Write-Host "`nTest 4: Custom interval (5 seconds)" -ForegroundColor Yellow
$process4 = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--duration", "0", "--interval", "5", "--verbose" -NoNewWindow -RedirectStandardOutput "cli_test_interval_out.log" -RedirectStandardError "cli_test_interval_err.log" -PassThru
Start-Sleep -Seconds 15
Stop-Process -Id $process4.Id -Force
if (Test-Path "cli_test_interval_out.log") {
    Write-Host "Output (should show refreshes every 5 seconds):" -ForegroundColor Green
    Get-Content "cli_test_interval_out.log" | Select-Object -Last 10
}

# Test 5: Help flag
Write-Host "`nTest 5: Help flag" -ForegroundColor Yellow
& .\bin\nosleep.exe --help

# Test 6: No arguments (should start tray mode)
Write-Host "`nTest 6: No arguments (should start tray mode - will run for 3 seconds)" -ForegroundColor Yellow
$process6 = Start-Process -FilePath ".\bin\nosleep.exe" -NoNewWindow -RedirectStandardOutput "cli_test_noargs_out.log" -RedirectStandardError "cli_test_noargs_err.log" -PassThru
Start-Sleep -Seconds 3
Stop-Process -Id $process6.Id -Force
if (Test-Path "cli_test_noargs_out.log") {
    Write-Host "Output:" -ForegroundColor Green
    Get-Content "cli_test_noargs_out.log" | Select-Object -First 5
}

Write-Host "`nAll CLI tests completed!" -ForegroundColor Cyan