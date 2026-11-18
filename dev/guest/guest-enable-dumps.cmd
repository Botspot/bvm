@echo off
rem Make Windows Error Reporting keep FULL crash dumps of dwm.exe (the RDP-session compositor crash loop).
rem Run once INSIDE Windows from an ADMIN prompt (or right-click > Run as administrator).
rem
rem v2: the folder is now writable by everyone. WerFault writes the dump as the account of the crashed process, and
rem dwm.exe in an RDP session runs as a restricted virtual account (Window Manager\DWM-2) that cannot write into a
rem folder created by an administrator: the first attempt left C:\CrashDumps empty for that reason.
set KEY=HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\dwm.exe
mkdir C:\CrashDumps 2>nul
icacls C:\CrashDumps /grant *S-1-1-0:(OI)(CI)F /T
reg add "%KEY%" /v DumpFolder /t REG_EXPAND_SZ /d "C:\CrashDumps" /f
reg add "%KEY%" /v DumpType /t REG_DWORD /d 2 /f
reg add "%KEY%" /v DumpCount /t REG_DWORD /d 4 /f
rem also the user-mode driver host that runs RdpIdd.dll (Remote Desktop indirect display driver): it crashed with
rem DriverFrameworks-UserMode 10121 about 45 s into an accelerated RDP session once the Triton adapter became usable
set KEY2=HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\WUDFHost.exe
reg add "%KEY2%" /v DumpFolder /t REG_EXPAND_SZ /d "C:\CrashDumps" /f
reg add "%KEY2%" /v DumpType /t REG_DWORD /d 2 /f
reg add "%KEY2%" /v DumpCount /t REG_DWORD /d 4 /f
echo.
echo Dump collection for dwm.exe and WUDFHost.exe enabled (full dumps, newest 4 kept) in C:\CrashDumps, writable by Everyone.
echo To disable later:  reg delete "%KEY%" /f
pause
