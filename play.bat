@echo off
rem Convenience wrapper: double-click to play with logging.
rem   play.bat         normal play
rem   play.bat menu    auto-advance to the main menu
rem   play.bat race    auto-drive into the Rio race
call "%~dp0tools\play.bat" %*
