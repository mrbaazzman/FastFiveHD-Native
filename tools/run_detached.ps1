param(
    [int]$Seconds = 600,
    [string]$Log = "build\v4long.log",
    [string]$Width = "1920",
    [string]$Height = "1080",
    [double]$Scale = 1.6
)
# Detached run: the port keeps running after this script returns, so a long session
# (e.g. the race load at 1080p) can be captured without holding the shell.
$env:FAST5_FAKE_GPU = "1"
$env:FAST5_LOG = $Log
$env:FAST5_WIDTH = $Width
$env:FAST5_HEIGHT = $Height
$env:FAST5_AUTO_MENU = "1"
$env:FAST5_AUTO_RACE = "1"
$env:FAST5_AUTO_SCALE = "$Scale"
$env:FAST5_TEST_MINIMIZED = "1"
$p = Start-Process -FilePath "build\fast5_win.exe" -WorkingDirectory (Get-Location) -PassThru
Write-Output "started pid=$($p.Id) for $Seconds s"
Start-Sleep -Seconds $Seconds
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    Write-Output "stopped"
}
