@echo off
rem Rebuilds Liquid and swaps the running copy without showing the lock screen.
rem The new copy is started by Task Scheduler, not by this window: anything started from a
rem terminal inside another app (like Claude) gets closed when that app updates or quits.
set ROOT=%~dp0..
cd /d "%ROOT%"
"%ROOT%\Liquid.exe" --quit-for-reload
call "%ROOT%\build.bat"
schtasks /create /tn "Liquid Shell Reload" /tr "\"%ROOT%\Liquid.exe\" --reloaded" /sc once /st 00:00 /sd 01/01/2000 /f >nul 2>&1
schtasks /run /tn "Liquid Shell Reload" >nul 2>&1
if errorlevel 1 start "" "%ROOT%\Liquid.exe" --reloaded
