@echo off
rem BVM GPU mode: the guest setup, in plain cmd on purpose. PowerShell needed about 2 minutes to start at logon on this VM, cmd starts at once.
rem It is for what BVM's non-RDP display modes want in Windows (the host folder as Z:, clipboard sync, the automatic login, power settings) and the GPU driver. NOT for
rem one-time personal settings (themes, wallpaper, debloating): those are in firstlogin.ps1, which runs once at the install and never again.
rem Two ways in. A new VM: firstlogin.ps1 copies the folder bvm of the unattended ISO to C:\bvm, adds firstboot.flag and the launcher, and runs this file at the end of the
rem install. An existing VM: gpu-firstboot (the host) copies this file to C:\bvm\guest-setup.cmd together with the files it installs (C:\bvm\payload\...), the Windows login (credentials.txt,
rem to repair the automatic login), inject-id.txt (the id of this injection) and a launcher in the all-users Startup folder. The launcher copies this file to
rem C:\Windows\Temp and runs the copy at the logon, in a visible window: this window IS the progress window. It prints everything it does and writes it to C:\bvm\setup.log
rem too (the host reads that log from the disk afterwards). Nothing else starts it: no group policy, no Run entry, no scheduled task, no PowerShell.
rem
rem The steps run in order. Each has a VERSION and a TEST. A step is skipped when its recorded version is current AND its test passes, and runs otherwise; it
rem counts as done when its test passes afterwards (recorded in HKLM\SOFTWARE\BVM-GPU\Steps). A step that fails only blocks the steps that need it.
rem  - A step that needs a restart (the GPU driver) restarts Windows after it newly succeeded, at most 3 times. The launcher runs this file again after the logon.
rem  - It gives up after 6 runs of the same injection.
rem  - When every step passed it removes itself (the launcher, C:\bvm\guest-setup.cmd, the payload, the credentials; the log and the step state stay), and if the host
rem    asked for it (C:\bvm\shutdown-when-done.flag, only for gpu-firstboot) it shuts Windows down after a 10 second countdown.
rem  - The colour of the window is the state: grey = running, yellow = restarting, green = finished or shutting down, red = failed (press a key to close it).
rem
rem To add or change a step: add a  call :step  line below and the labels :test_NAME and :run_NAME (NAME is the id with - turned into _). Bump the version of a
rem changed step. Passwords with a double quote in them cannot be used for the automatic login repair, and the log lines of this file must not contain ! or ^
rem For testing, the folder with credentials.txt, payload, setup.log can be given as the first argument.
setlocal enabledelayedexpansion
set "BASE=%~1"
if "%BASE%"=="" set "BASE=C:\bvm"
set "PAYLOAD=%BASE%\payload"
set "LOG=%BASE%\setup.log"
set "STATE=HKLM\SOFTWARE\BVM-GPU"
set "WINLOGON=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
set "RUNKEY=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run"
set "SMBPARAMS=HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters"
set "SHAREIP=\\10.0.2.4\qemu"
set "LAUNCHER=%ProgramData%\Microsoft\Windows\Start Menu\Programs\StartUp\bvm-guest-setup.cmd"
set "LOCK=C:\Windows\Temp\bvm-setup.lock"

rem only one run at a time: the main part holds a lock file (a redirect to a file that another process has open fails, and then nothing runs)
call :main 2>>"%BASE%\setup.errors.log" 9>"%LOCK%" || exit /b 0
del "%LOCK%" 2>nul
rem the copy that the launcher made deletes itself (the (goto) trick lets a batch file delete itself while it runs)
if /i "%~dp0"=="C:\Windows\Temp\" (
  (goto) 2>nul & del "%~f0"
)
exit /b 0

rem ===========================================================================================================================
:main
title BVM: GPU setup
if not exist "%BASE%" mkdir "%BASE%"
echo.
echo   BVM: getting Windows ready for GPU mode
echo   This window shows what the setup is doing. Windows may restart, and it shuts down by itself when everything is done.
echo   Please do not close this window and do not shut Windows down yourself.
echo.
call :info

rem fltmc only works for an administrator (in a BVM VM the user is one, and UAC is off)
fltmc >nul 2>&1
if errorlevel 1 (
  call :log "not elevated: this setup needs an administrator, so it cannot run"
  color 0C
  call :keep_open
  exit /b 1
)

rem give up after 6 runs of the same injection: a step that keeps failing must not retry for ever
set "SELFID=standalone"
if exist "%BASE%\inject-id.txt" set /p SELFID=<"%BASE%\inject-id.txt"
set att=0
for /f "tokens=2*" %%a in ('reg query "%STATE%" /v Attempts 2^>nul ^| findstr /c:"REG_SZ"') do (
  for /f "tokens=1,2 delims=:" %%x in ("%%b") do if "%%x"=="!SELFID!" set /a att=%%y
)
set /a att+=1
reg add "%STATE%" /v Attempts /t REG_SZ /d "!SELFID!:!att!" /f >nul
for /f "delims=" %%v in ('ver') do if not "%%v"=="" set "OSVER=%%v"
if !att! gtr 6 (
  call :log "giving up: the steps kept failing for 6 runs of this injection (see the earlier lines of this log). Run bvm gpu-firstboot on the host to try again"
  call :remove_self "gave up"
  color 0C
  call :keep_open
  exit /b 1
)

call :log "===== guest-setup start (run !att!, as %USERNAME%, !OSVER!)"
set ran=0
set failed=
set reboot=0
call :step autologon 1 "Winlogon auto-login with the credentials of the host (repairs a stale or used up auto-login)"
call :step smb-client 1 "allow password-less (guest) SMB logons without signing, for the QEMU share" ""
call :step smb-map 3 "map the host share as Z: for every user at logon (Run entry, no window)" smb-client
call :step serial-driver 1 "VirtIO serial driver (the SPICE agent needs it for the clipboard)" ""
call :step spice-agent 1 "SPICE guest agent (clipboard between the host and Windows)" serial-driver
call :step gpu-driver 1 "Triton viogpu3d GPU driver" ""
call :step power 1 "no hibernation or fast startup, no sleep or display timeout, high performance plan" ""
if defined failed (set "flist=!failed!") else set "flist=none"
call :log "===== guest-setup end: !ran! step(s) ran, failed: !flist!"

rem a restart after a step that needs one, at most 3 times
set rc=0
for /f "tokens=3" %%v in ('reg query "%STATE%" /v RebootCount 2^>nul ^| findstr /c:"REG_DWORD"') do set /a rc=%%v
if !reboot!==1 if !rc! lss 3 (
  set /a rc+=1
  reg add "%STATE%" /v RebootCount /t REG_DWORD /d !rc! /f >nul
  color 0E
  call :log "a step needs a restart: restarting now. This setup runs again after the restart and checks everything again"
  echo.
  echo   Windows restarts now to finish the installation. This window opens again afterwards.
  shutdown /r /t 0 /f /c "BVM guest setup: restarting to finish installing the GPU driver"
  call :wait 60
  exit /b 0
)

if defined failed (
  color 0C
  echo.
  echo   The setup did not finish cleanly. The lines above say why; %LOG% has all of them.
  echo   Windows keeps working. Run bvm gpu-firstboot on the host to try again.
  call :keep_open
  exit /b 1
)
if !reboot!==1 (
  call :log "the GPU driver needs a restart, but 3 restarts were done already: not restarting again"
  color 0C
  call :keep_open
  exit /b 1
)

rem every step passed
reg add "%STATE%" /v RebootCount /t REG_DWORD /d 0 /f >nul
reg add "%STATE%" /v Attempts /t REG_SZ /d "!SELFID!:0" /f >nul
call :log "all steps passed"
set stop=0
if exist "%BASE%\shutdown-when-done.flag" (
  del /f /q "%BASE%\shutdown-when-done.flag"
  set stop=1
  call :log "the host asked for a shutdown when the setup is done (first GPU boot): shutting down in 10 seconds"
)
call :remove_self "all steps passed"
color 0A
echo.
if !stop!==1 (
  echo   Everything is done. Windows shuts down by itself in 10 seconds.
  shutdown /s /t 10 /f /c "BVM guest setup finished"
  call :wait 60
) else (
  echo   The setup finished. This window closes in 10 seconds.
  call :wait 10
)
exit /b 0

rem ===========================================================================================================================
:keep_open
rem a failed setup stays on the screen until a key is pressed. During the first boot of a new VM (firstboot.flag, written by firstlogin.ps1) nobody is expected to be
rem watching, and the install must go on and shut down, so there it only waits a minute. The log has the reason either way.
if exist "%BASE%\firstboot.flag" (
  echo   The installation of the VM goes on in 60 seconds. Run bvm gpu-firstboot on the host later to try this setup again.
  call :wait 60
) else (
  echo   Press a key to close this window.
  pause >nul
)
exit /b 0

:wait
rem wait %1 seconds (ping waits one second between its tries)
set /a tries=%~1+1
ping -n !tries! 127.0.0.1 >nul 2>&1
exit /b 0

:log
rem one line to the window and to the log
set "msg=%~1"
set "stamp=%date% %time:~0,8%"
echo   !stamp! !msg!
>>"%LOG%" echo !stamp! !msg!
exit /b 0

:logcmd
rem run the command %* and show and log its output line by line
setlocal disabledelayedexpansion
for /f "delims=" %%L in ('%* 2^>^&1') do (
  echo   %%L
  >>"%LOG%" echo %%L
)
endlocal
exit /b 0

:step
rem :step id version "description" [id of a step that this one needs]
set "id=%~1"
set "sver=%~2"
set "desc=%~3"
set "needs=%~4"
set "key=%id:-=_%"
if defined needs (
  set "nkey=%needs:-=_%"
  if not defined ok_!nkey! (
    call :log "[%id%] blocked: needs %needs%"
    set "failed=!failed! %id%"
    exit /b 0
  )
)
set done=0
for /f "tokens=3" %%v in ('reg query "%STATE%\Steps" /v %id% 2^>nul ^| findstr /c:"REG_DWORD"') do set /a done=%%v
if !done! geq %sver% (
  call :test_%key%
  if not errorlevel 1 (
    call :log "[%id%] ok (already done)"
    set "ok_%key%=1"
    exit /b 0
  )
)
call :log "[%id%] running: %desc%"
set /a ran+=1
call :run_%key%
call :test_%key%
if errorlevel 1 (
  set "failed=!failed! %id%"
  call :log "[%id%] FAILED: the step ran but its check still fails"
) else (
  reg add "%STATE%\Steps" /v %id% /t REG_DWORD /d %sver% /f >nul
  set "ok_%key%=1"
  call :log "[%id%] done"
  if "%id%"=="gpu-driver" set reboot=1
)
exit /b 0

:remove_self
rem remove everything that this setup put on the disk, except the log and the step state. The launcher, the files and the payload; this running copy goes at the end.
call :log "removing the setup (%~1): the launcher, C:\bvm\guest-setup.cmd, payload and credentials (the log and the step state stay)"
del /f /q "%LAUNCHER%" "%BASE%\guest-setup.cmd" "%BASE%\guest-autologon.cmd" "%BASE%\inject-id.txt" "%BASE%\credentials.txt" "%BASE%\firstboot.flag" 2>nul
rd /s /q "%PAYLOAD%" 2>nul
exit /b 0

:info
echo   --- debug information
echo   %COMPUTERNAME%: %USERNAME% at %date% %time:~0,8%
for /f "delims=" %%v in ('ver') do if not "%%v"=="" echo   %%v
for %%f in (credentials.txt shutdown-when-done.flag) do (
  if exist "%BASE%\%%f" (echo   %%f: present) else (echo   %%f: not there)
)
for /f "tokens=1,3" %%a in ('reg query "%STATE%\Steps" 2^>nul ^| findstr /c:"REG_DWORD"') do echo   step %%a done, version %%b
for /f "tokens=1,* delims=:" %%a in ('pnputil /enum-devices /class Display 2^>nul ^| findstr /i /c:"Device Description" /c:"Status"') do echo   display adapter %%a:%%b
echo.
exit /b 0

rem ===========================================================================================================================
rem the steps. A test exits with 0 when the step is done, with 1 when it is not. The labels that use passwords turn delayed expansion off, so that ! in a password survives.

:test_autologon
setlocal disabledelayedexpansion
if not exist "%BASE%\credentials.txt" exit /b 0
set "CRED_USER="
set "CRED_PASS="
for /f "usebackq delims=" %%L in ("%BASE%\credentials.txt") do (
  if not defined CRED_USER (set "CRED_USER=%%L") else if not defined CRED_PASS set "CRED_PASS=%%L"
)
set "cur="
for /f "tokens=2*" %%a in ('reg query "%WINLOGON%" /v AutoAdminLogon 2^>nul ^| findstr /c:"REG_SZ"') do set "cur=%%b"
if not "%cur%"=="1" exit /b 1
set "cur="
for /f "tokens=2*" %%a in ('reg query "%WINLOGON%" /v DefaultUserName 2^>nul ^| findstr /c:"REG_SZ"') do set "cur=%%b"
if not "%cur%"=="%CRED_USER%" exit /b 1
set "cur="
for /f "tokens=2*" %%a in ('reg query "%WINLOGON%" /v DefaultPassword 2^>nul ^| findstr /c:"REG_SZ"') do set "cur=%%b"
if not "%cur%"=="%CRED_PASS%" exit /b 1
reg query "%WINLOGON%" /v AutoLogonCount >nul 2>&1 && exit /b 1
set "cur="
for /f "tokens=3" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device" /v DevicePasswordLessBuildVersion 2^>nul ^| findstr /c:"REG_DWORD"') do set "cur=%%a"
if defined cur if not "%cur%"=="0x0" exit /b 1
exit /b 0

:run_autologon
setlocal disabledelayedexpansion
rem no login supplied: leave the automatic login that BVM set up alone
if not exist "%BASE%\credentials.txt" exit /b 0
set "CRED_USER="
set "CRED_PASS="
for /f "usebackq delims=" %%L in ("%BASE%\credentials.txt") do (
  if not defined CRED_USER (set "CRED_USER=%%L") else if not defined CRED_PASS set "CRED_PASS=%%L"
)
reg add "%WINLOGON%" /v AutoAdminLogon /t REG_SZ /d 1 /f >nul
reg add "%WINLOGON%" /v DefaultUserName /t REG_SZ /d "%CRED_USER%" /f >nul
reg add "%WINLOGON%" /v DefaultPassword /t REG_SZ /d "%CRED_PASS%" /f >nul
reg add "%WINLOGON%" /v DefaultDomainName /t REG_SZ /d "%COMPUTERNAME%" /f >nul
reg delete "%WINLOGON%" /v AutoLogonCount /f >nul 2>&1
rem "Windows Hello only" sign-in hides and ignores the password auto-login of Windows 11
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device" /v DevicePasswordLessBuildVersion /t REG_DWORD /d 0 /f >nul
exit /b 0

:test_smb_client
reg query "%SMBPARAMS%" /v AllowInsecureGuestAuth 2>nul | findstr /c:"0x1" >nul || exit /b 1
reg query "%SMBPARAMS%" /v RequireSecuritySignature 2>nul | findstr /c:"0x0" >nul || exit /b 1
exit /b 0

:run_smb_client
reg add "%SMBPARAMS%" /v AllowInsecureGuestAuth /t REG_DWORD /d 1 /f >nul
reg add "%SMBPARAMS%" /v RequireSecuritySignature /t REG_DWORD /d 0 /f >nul
exit /b 0

:test_smb_map
reg query "%RUNKEY%" /v BVM-MapShare 2>nul | findstr /c:"conhost.exe --headless" | findstr /c:"10.0.2.4" >nul
exit /b %errorlevel%

:run_smb_map
rem a mapped drive has to be made in the session of each user, so an all-users Run entry does it at every logon. conhost --headless: no console window flashes up
reg add "%RUNKEY%" /v BVM-MapShare /t REG_SZ /d "conhost.exe --headless cmd.exe /c net use Z: %SHAREIP% /persistent:no >nul 2>&1" /f >nul
exit /b 0

:test_serial_driver
pnputil /enum-drivers 2>nul | findstr /i /c:"vioser.inf" >nul
exit /b %errorlevel%

:run_serial_driver
if not exist "%PAYLOAD%\vioserial-arm64\vioser.inf" (
  call :log "missing payload file %PAYLOAD%\vioserial-arm64\vioser.inf"
  exit /b 1
)
call :logcmd pnputil /add-driver "%PAYLOAD%\vioserial-arm64\vioser.inf" /install
exit /b 0

:test_spice_agent
set "AGENTDIR=C:\Program Files\SPICE Agent"
fc /b "%AGENTDIR%\vdagent.exe" "%PAYLOAD%\vdagent-arm64\vdagent.exe" >nul 2>&1 || exit /b 1
fc /b "%AGENTDIR%\vdservice.exe" "%PAYLOAD%\vdagent-arm64\vdservice.exe" >nul 2>&1 || exit /b 1
rem (starts automatically: it does not have to be running yet, this setup runs early)
sc qc vdservice 2>nul | findstr /c:"AUTO_START" >nul || exit /b 1
exit /b 0

:run_spice_agent
set "AGENTDIR=C:\Program Files\SPICE Agent"
if not exist "%PAYLOAD%\vdagent-arm64\vdservice.exe" (
  call :log "missing payload file %PAYLOAD%\vdagent-arm64\vdservice.exe"
  exit /b 1
)
set "HAVE=1"
sc query vdservice >nul 2>&1 || set "HAVE=0"
if "%HAVE%"=="1" (
  sc stop vdservice >nul 2>&1
  call :wait 2
)
taskkill /f /im vdagent.exe >nul 2>&1
if not exist "%AGENTDIR%" mkdir "%AGENTDIR%"
copy /y "%PAYLOAD%\vdagent-arm64\vdagent.exe" "%AGENTDIR%\" >nul
copy /y "%PAYLOAD%\vdagent-arm64\vdservice.exe" "%AGENTDIR%\" >nul
if "%HAVE%"=="0" call :logcmd "%AGENTDIR%\vdservice.exe" install
sc config vdservice start= auto >nul 2>&1
net start vdservice >nul 2>&1
exit /b 0

:test_gpu_driver
pnputil /enum-drivers 2>nul | findstr /i /c:"viogpu3d.inf" >nul
exit /b %errorlevel%

:run_gpu_driver
if not exist "%PAYLOAD%\viogpu3d\viogpu3d.inf" (
  call :log "missing payload file %PAYLOAD%\viogpu3d\viogpu3d.inf"
  exit /b 1
)
call :logcmd pnputil /add-driver "%PAYLOAD%\viogpu3d\viogpu3d.inf" /install
exit /b 0

:test_power
dir /a C:\hiberfil.sys >nul 2>&1 && exit /b 1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled 2>nul | findstr /c:"0x0" >nul || exit /b 1
exit /b 0

:run_power
powercfg /h off >nul 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled /t REG_DWORD /d 0 /f >nul
powercfg /setactive SCHEME_MIN >nul 2>&1
for %%s in (monitor-timeout-ac monitor-timeout-dc standby-timeout-ac standby-timeout-dc disk-timeout-ac disk-timeout-dc) do powercfg /change %%s 0 >nul 2>&1
exit /b 0
