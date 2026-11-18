@echo off
rem BVM GPU mode, guest side: repair a broken automatic login WITHOUT anybody logging in.
rem Injected as C:\bvm\guest-autologon.cmd and registered as a local Group Policy computer startup script, which Windows runs as SYSTEM at every boot, before the
rem login screen accepts anybody (the setup itself, guest-setup.cmd, runs from the Startup folder, so only after a logon, which a broken login would never give it).
rem If the login of C:\bvm\credentials.txt is not what Winlogon would use, it writes it and restarts Windows once. If it is fine, or the credentials are gone (the
rem setup deleted them when it finished), it does nothing. The host removes this script and its policy entry when the setup is done. Log: C:\bvm\autologon.log
setlocal disabledelayedexpansion
set "BASE=C:\bvm"
set "LOG=%BASE%\autologon.log"
set "WINLOGON=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
set "HELLO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device"
set "STATE=HKLM\SOFTWARE\BVM-GPU"

if not exist "%BASE%\credentials.txt" exit /b 0

set "CRED_USER="
set "CRED_PASS="
for /f "usebackq delims=" %%L in ("%BASE%\credentials.txt") do (
  if not defined CRED_USER (set "CRED_USER=%%L") else if not defined CRED_PASS set "CRED_PASS=%%L"
)
if not defined CRED_USER exit /b 0

rem is the login fine? (the same checks as the autologon step of guest-setup.cmd, plus the "Windows Hello only" setting)
set "broken="
set "cur="
for /f "tokens=2*" %%a in ('reg query "%WINLOGON%" /v AutoAdminLogon 2^>nul ^| findstr /c:"REG_SZ"') do set "cur=%%b"
if not "%cur%"=="1" set "broken=AutoAdminLogon is not 1"
set "cur="
for /f "tokens=2*" %%a in ('reg query "%WINLOGON%" /v DefaultUserName 2^>nul ^| findstr /c:"REG_SZ"') do set "cur=%%b"
if not "%cur%"=="%CRED_USER%" set "broken=DefaultUserName is not %CRED_USER%"
set "cur="
for /f "tokens=2*" %%a in ('reg query "%WINLOGON%" /v DefaultPassword 2^>nul ^| findstr /c:"REG_SZ"') do set "cur=%%b"
if not "%cur%"=="%CRED_PASS%" set "broken=DefaultPassword is missing or different"
reg query "%WINLOGON%" /v AutoLogonCount >nul 2>&1 && set "broken=AutoLogonCount is set (a limited number of automatic logins)"
set "cur="
for /f "tokens=3" %%a in ('reg query "%HELLO%" /v DevicePasswordLessBuildVersion 2^>nul ^| findstr /c:"REG_DWORD"') do set "cur=%%a"
if defined cur if not "%cur%"=="0x0" set "broken=sign-in is set to Windows Hello only"

if not defined broken (
  reg delete "%STATE%" /v AutologonReboots /f >nul 2>&1
  exit /b 0
)

set "stamp=%date% %time:~0,8%"
>>"%LOG%" echo %stamp% broken automatic login: %broken%

rem at most 2 repair restarts in a row: if the repair does not hold, a restart loop would be worse than the login screen
set "n=0"
for /f "tokens=3" %%a in ('reg query "%STATE%" /v AutologonReboots 2^>nul ^| findstr /c:"REG_DWORD"') do set /a n=%%a
if %n% geq 2 (
  >>"%LOG%" echo %stamp% repaired %n% times already and it did not hold: not restarting again
  exit /b 1
)

reg add "%WINLOGON%" /v AutoAdminLogon /t REG_SZ /d 1 /f >nul
reg add "%WINLOGON%" /v DefaultUserName /t REG_SZ /d "%CRED_USER%" /f >nul
reg add "%WINLOGON%" /v DefaultPassword /t REG_SZ /d "%CRED_PASS%" /f >nul
reg add "%WINLOGON%" /v DefaultDomainName /t REG_SZ /d "%COMPUTERNAME%" /f >nul
reg delete "%WINLOGON%" /v AutoLogonCount /f >nul 2>&1
reg add "%HELLO%" /v DevicePasswordLessBuildVersion /t REG_DWORD /d 0 /f >nul
set /a n+=1
reg add "%STATE%" /v AutologonReboots /t REG_DWORD /d %n% /f >nul

>>"%LOG%" echo %stamp% automatic login written for %CRED_USER%: restarting Windows (repair %n% of 2)
shutdown /r /t 3 /f /c "BVM guest setup: restarting to repair the automatic login"
exit /b 0
