@echo off
rem Switches every renderer optimization off (the disk shader store stays on).
> "%~dp0toggles.txt" echo 14987979559889010687
echo Done. This session may now draw wrongly: restart the game (start.bat) after checking.
pause
