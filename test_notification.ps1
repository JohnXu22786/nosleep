# Test notification system (timer expiration)
$ErrorActionPreference = "Stop"
$env:NOSLEEP_DEBUG = "1"

$exePath = "bin\nosleep.exe"
$stdoutLog = "notification_stdout.log"
$stderrLog = "notification_stderr.log"
$combinedLog = "notification_test.log"

Write-Host "Starting nosleep with 1 minute auto-start..."
Write-Host "Output will be logged to $stdoutLog (stdout) and $stderrLog (stderr)"

# Start process with stdout and stderr redirected to separate files
$process = Start-Process -FilePath $exePath -PassThru -NoNewWindow `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog

Write-Host "Process started with PID $($process.Id)"
Write-Host "Waiting 70 seconds for timer expiration..."

# Wait 70 seconds
Start-Sleep -Seconds 70

Write-Host "Checking if process is still running..."
if (!$process.HasExited) {
    Write-Host "Process still running, killing it..."
    Stop-Process -Id $process.Id -Force
    Write-Host "Process killed."
} else {
    Write-Host "Process already exited."
}

# Combine logs for easier reading
Copy-Item $stdoutLog $combinedLog -Force
Add-Content $combinedLog "`n=== STDERR ==="
Get-Content $stderrLog | Add-Content $combinedLog

# Check for notification messages
Write-Host "`n--- Checking for notification messages ---"
$stdoutContent = Get-Content $stdoutLog -ErrorAction SilentlyContinue
$stderrContent = Get-Content $stderrLog -ErrorAction SilentlyContinue

$notificationFound = $false
$timerExpiredFound = $false

foreach ($line in $stdoutContent + $stderrContent) {
    if ($line -match "\[Notification\].*Time's up") {
        Write-Host "Found 'Time''s up!' notification: $line"
        $notificationFound = $true
    }
    if ($line -match "Duration reached") {
        Write-Host "Found duration reached debug message: $line"
        $timerExpiredFound = $true
    }
    if ($line -match "NIN_BALLOONSHOW") {
        Write-Host "Found balloon show notification: $line"
    }
}

if (-not $notificationFound) {
    Write-Host "No 'Time''s up!' notification found in logs"
}

if (-not $timerExpiredFound) {
    Write-Host "No duration reached debug message found"
}

# Show tail of combined log
Write-Host "`n--- Last 30 lines of combined log ---"
Get-Content $combinedLog -Tail 30