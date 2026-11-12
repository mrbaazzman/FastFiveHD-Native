@echo off
setlocal
rem ============================================================================
rem  play.bat - launch the Fast Five HD port with the persistent session log.
rem
rem    play.bat              normal play, log to build\fast5.log
rem    play.bat menu         auto-advance the title into the main menu
rem    play.bat race         auto-drive all the way into the Rio race
rem
rem  Close the game window when you are done. The log is flushed continuously,
rem  so it stays readable even if the process is killed.
rem ============================================================================

cd /d "%~dp0.."

rem  The window is 1920x1080 by default (borderless when that fills the screen).
rem  Set FAST5_WIDTH / FAST5_HEIGHT for another presentation size, e.g.
rem      set FAST5_WIDTH=1280 & set FAST5_HEIGHT=720
rem  Set FAST5_UI_FIT=letterbox for an undistorted (uniformly scaled) 2D layer.
set FAST5_FAKE_GPU=1
set FAST5_LOG=build\fast5.log
set FAST5_AUTO_MENU=
set FAST5_AUTO_RACE=
rem Log which gamepad buttons the engine queries (nitro / camera / rewind / look-back discovery).
set FAST5_PAD_TRACE=1

if /i "%~1"=="menu" set FAST5_AUTO_MENU=1
if /i "%~1"=="race" set FAST5_AUTO_MENU=1
if /i "%~1"=="race" set FAST5_AUTO_RACE=1

echo logging to %FAST5_LOG%
echo FAST5_FAKE_GPU=1  FAST5_AUTO_MENU=%FAST5_AUTO_MENU%  FAST5_AUTO_RACE=%FAST5_AUTO_RACE%
echo.
echo Controls:
echo   mouse        = touch (menus, buttons)
echo   arrow left / right = steer (tilt)
echo   arrow up / down    = pitch (tilt)
echo   Enter / Space      = touch at screen centre
echo   Alt+F4             = exit
echo.
echo Resolution: the game runs at its own 800x480 layout and is presented in the
echo window (1920x1080 by default). Set FAST5_WIDTH / FAST5_HEIGHT to change it.
echo.
echo Close the game window when finished.
echo.

build\fast5_win.exe
set CODE=%ERRORLEVEL%

echo.
echo ---------------------------------------------------------------
echo exit code: %CODE%
if exist "%FAST5_LOG%" (
    for %%A in ("%FAST5_LOG%") do echo log: %%~fA  ^(%%~zA bytes^)
    echo.
    echo --- state transitions ---
    findstr /b /c:"STATE " /c:"state " "%FAST5_LOG%"
)
echo ---------------------------------------------------------------
endlocal
