param(
    [string]$Title = "Fast Five HD",
    [string]$Out = "build/window_capture.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
public class WinCap {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
}
"@
$p = Get-Process | Where-Object { $_.MainWindowTitle -eq $Title } | Select-Object -First 1
if (-not $p) { Write-Output "window not found: $Title"; exit 1 }
[WinCap]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
[WinCap]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 600
$r = New-Object RECT
if (-not [WinCap]::GetWindowRect($p.MainWindowHandle, [ref]$r)) { Write-Output "GetWindowRect failed"; exit 1 }
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
Write-Output "window $($r.Left),$($r.Top) ${w}x${h}"
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$bmp.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Output "saved $Out"
