param(
    [int]$Seconds = 45,
    [string]$Out = "build/window_capture.png",
    [string]$Width = "800",
    [string]$Height = "480",
    [string]$Extra = ""
)
# Run the port with a visible window and grab the real window content with PrintWindow.
# This is the ground truth for "what the player sees" — the in-engine glReadPixels capture
# is a different path and can differ (e.g. a Y-flipped readback).
$env:FAST5_FAKE_GPU = "1"
$env:FAST5_LOG = "build\fast5.log"
$env:FAST5_WIDTH = $Width
$env:FAST5_HEIGHT = $Height
$env:FAST5_AUTO_MENU = "1"
Remove-Item Env:FAST5_TEST_MINIMIZED -ErrorAction SilentlyContinue
if ($Extra -ne "") { $Extra.Split(";") | ForEach-Object { $kv = $_.Split("="); Set-Item -Path ("Env:" + $kv[0]) -Value $kv[1] } }

$p = Start-Process -FilePath "build\fast5_win.exe" -PassThru
Write-Output "started pid=$($p.Id); waiting $Seconds s"
Start-Sleep -Seconds $Seconds
& powershell -ExecutionPolicy Bypass -File tools\capture_printwindow.ps1 -Title "Fast Five HD" -Out $Out
Start-Sleep -Seconds 1
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "done"
