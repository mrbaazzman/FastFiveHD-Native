$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$env:FAST5_FAKE_GPU   = '1'
$env:FAST5_AUTO_MENU  = '1'
$env:FAST5_AUTO_RACE  = '1'
$env:FAST5_TEX_AUDIT  = '1'
$env:FAST5_TEXTURE_TRACE = '1'
$env:FAST5_LOG = 'build\audit_race.log'

$out = 'build\audit_run.err'
Remove-Item $out -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath 'build\fast5_win.exe' -RedirectStandardError $out -PassThru -WorkingDirectory $root
Write-Host "started pid $($proc.Id)"

$deadline = (Get-Date).AddSeconds(600)
$stopped  = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if ($proc.HasExited) { Write-Host "exited early code=$($proc.ExitCode)"; $stopped = $true; break }
    if (Test-Path $out) {
        $t = Get-Content $out -Raw -ErrorAction SilentlyContinue
        if ($t -and $t -match 'capture frame 3600') {
            Write-Host 'reached frame 3600, stopping'
            $stopped = $true
            break
        }
    }
}
if (-not $stopped) { Write-Host 'deadline reached, stopping' }
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Get-ChildItem build\audit_run.err, build\audit_race.log -ErrorAction SilentlyContinue |
    Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
