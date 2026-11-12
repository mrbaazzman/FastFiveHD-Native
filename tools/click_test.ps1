param(
    [int]$X = 1600,
    [int]$Y = 900,
    [int]$Seconds = 45
)
# Click at a window position and show how the port maps it into the guest's design space.
# The guest engine runs at 800x480, so the touch it receives must be the design-space point.
$env:FAST5_FAKE_GPU = "1"
$env:FAST5_LOG = "build\fast5.log"
$env:FAST5_WIDTH = "1920"
$env:FAST5_HEIGHT = "1080"
$env:FAST5_POINTER_TRACE = "1"
Remove-Item Env:FAST5_TEST_MINIMIZED -ErrorAction SilentlyContinue

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Mouse {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
}
"@
$p = Start-Process -FilePath "build\fast5_win.exe" -PassThru
Write-Output "started pid=$($p.Id); waiting $Seconds s"
Start-Sleep -Seconds $Seconds
[Mouse]::SetCursorPos($X, $Y) | Out-Null
Start-Sleep -Milliseconds 300
[Mouse]::mouse_event([Mouse]::LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 120
[Mouse]::mouse_event([Mouse]::LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Seconds 2
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Select-String -Path "build\fast5.log" -Pattern "pointer:|touch press" | Select-Object -Last 6 | ForEach-Object { $_.Line }
