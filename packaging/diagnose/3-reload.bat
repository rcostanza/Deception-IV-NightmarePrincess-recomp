@echo off
rem Uploads every texture and buffer page the GPU did not write again from game memory.
type nul > "%~dp0reload.trigger"
echo Done. This session may now draw wrongly: restart the game (start.bat) after checking.
pause
