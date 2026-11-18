@echo off
rem Stop the Windows Update / TiWorker CPU burn seen in update-diag.txt (README item 143).
rem Run INSIDE Windows from an ADMINISTRATOR prompt:
rem   guest-update-fix.cmd           clear the corrupt update download cache + disable the Secure Boot servicing task, restart WU
rem   guest-update-fix.cmd pause     the same, then switch automatic updates OFF (services disabled, policy NoAutoUpdate=1)
rem   guest-update-fix.cmd resume    undo "pause"
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)

if /i "%1"=="resume" goto resume

echo [1/4] Stopping the update services...
net stop usosvc /y >nul 2>&1
net stop wuauserv /y >nul 2>&1
net stop bits /y >nul 2>&1
sc stop trustedinstaller >nul 2>&1
timeout /t 3 /nobreak >nul

echo [2/4] Clearing C:\Windows\SoftwareDistribution\Download (holds the corrupt Windows11.0-KB5127690-arm64-NDP481.cab, error 0xCA00A005)...
rd /s /q C:\Windows\SoftwareDistribution\Download >nul 2>&1
mkdir C:\Windows\SoftwareDistribution\Download >nul 2>&1

echo [3/4] Disabling the Secure Boot servicing task (TPM-WMI event 1796 every ~15 min; Secure Boot is not enabled in this VM)...
schtasks /Change /TN "\Microsoft\Windows\PI\Secure-Boot-Update" /Disable

if /i "%1"=="pause" goto pause

echo [4/4] Starting Windows Update again (it will re-download KB5127690; if it fails with 0xCA00A005 again run this script with "pause")...
net start wuauserv
goto done

:pause
echo [4/4] Switching automatic updates off...
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU" /v NoAutoUpdate /t REG_DWORD /d 1 /f
sc config wuauserv start= disabled
sc config UsoSvc start= disabled
echo Updates are OFF. Undo with: guest-update-fix.cmd resume
goto done

:resume
reg delete "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU" /v NoAutoUpdate /f
sc config wuauserv start= demand
sc config UsoSvc start= auto
net start wuauserv
echo Updates are back on.

:done
echo.
echo Done. Watch Task Manager for a few minutes (Details tab: TiWorker.exe, svchost.exe running wuauserv, MsMpEng.exe).
