Write-Host "Testing system tray mode comprehensively..." -ForegroundColor Cyan

# Set debug environment variable
$env:NOSLEEP_DEBUG='1'

# Test 1: Start tray mode and test right-click menu (will run for 15 seconds)
Write-Host "`nTest 1: Starting tray mode (15 seconds)" -ForegroundColor Yellow
$process = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--tray" -NoNewWindow -PassThru
Start-Sleep -Seconds 5

Write-Host "Tray should be running. Right-click should show menu with options:" -ForegroundColor Green
Write-Host "  30 minutes, 1 hour, 2 hours, Custom..., Indefinite, Stop, Exit" -ForegroundColor Green
Write-Host "Please manually test the right-click menu on the tray icon." -ForegroundColor Yellow
Write-Host "The tray will run for 10 more seconds..." -ForegroundColor Yellow

Start-Sleep -Seconds 10
Stop-Process -Id $process.Id -Force
Write-Host "Test 1 completed"

# Test 2: Start tray with prevent-display and away-mode flags
Write-Host "`nTest 2: Tray with prevent-display and away-mode flags (10 seconds)" -ForegroundColor Yellow
$process2 = Start-Process -FilePath ".\bin\nosleep.exe" -ArgumentList "--tray", "--prevent-display", "--away-mode" -NoNewWindow -PassThru
Start-Sleep -Seconds 10
Stop-Process -Id $process2.Id -Force
Write-Host "Test 2 completed"

# Test 3: Test custom duration dialog (this would require UI automation, just start and stop)
Write-Host "`nTest 3: Testing tray mode basic functionality (5 seconds)" -ForegroundColor Yellow
$process3 = Start-Process -FilePath ".\bin\nosleep.exe" -NoNewWindow -RedirectStandardOutput "tray_test_out.log" -RedirectStandardError "tray_test_err.log" -PassThru
Start-Sleep -Seconds 5
Stop-Process -Id $process3.Id -Force

if (Test-Path "tray_test_out.log") {
    Write-Host "Output:" -ForegroundColor Green
    Get-Content "tray_test_out.log" | Select-Object -First 10
}
if (Test-Path "tray_test_err.log") {
    Write-Host "Errors:" -ForegroundColor Red
    Get-Content "tray_test_err.log" | Select-Object -First 10
}

Write-Host "`nNote: For full tray testing, you need to manually:" -ForegroundColor Cyan
Write-Host "1. Right-click tray icon and select each menu option" -ForegroundColor Cyan
Write-Host "2. Verify countdown icons change (0-59)" -ForegroundColor Cyan
Write-Host "3. Check notifications appear when timer expires" -ForegroundColor Cyan

Write-Host "`nAll automated tray tests completed!" -ForegroundColor Cyan