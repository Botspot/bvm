# Define log file path
$LogFile = "C:\Windows\Temp\firstlogin_log.txt"

# Start logging
Start-Transcript -Path $LogFile -Append

# Function to log and execute commands
function Execute-Command {
    param (
        [string]$Command
    )
    Write-Output "`n========== Executing: $Command =========="
    $Output = Invoke-Expression $Command 2>&1
    $Output
    if ($LASTEXITCODE -ne 0) {
        Write-Output "ERROR: Command failed with exit code $LASTEXITCODE"
    }
    Write-Output "========== End of Output ==========`n"
}

Write-Output "BVM setting up this Virtual Machine... please do not close this window! This VM will shutdown once done."

Start-Sleep -Seconds 5

# Disable Hibernation
Execute-Command "powercfg -H OFF"

# Disable recovery environment partition
Execute-Command "reagentc /disable"

# qemu guest agent
Execute-Command 'msiexec /i E:\guest-agent\qemu-ga-x86_64.msi /quiet /passive /qn'

# Enable rounded corners
Execute-Command 'reg add HKLM\SOFTWARE\Microsoft\Windows\Dwm /v ForceEffectMode /t REG_DWORD /d 2 /f'

# Enable RDP
Execute-Command 'reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 0 /f'
Execute-Command 'reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp" /v UserAuthentication /t REG_DWORD /d 0 /f'

# Allow incoming RDP connections through the firewall
Execute-Command 'netsh advfirewall firewall add rule name="Open Port 3389" dir=in action=allow protocol=TCP localport=3389'

# Configure and start Remote Desktop Service
Execute-Command 'sc config TermService start=auto'
Execute-Command 'net start TermService'

# Logout account after RDP connection is inactive for 1 minute
# This is now skipped due to https://github.com/Botspot/bvm/issues/48
#Execute-Command 'reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services" /v MaxDisconnectionTime /t REG_DWORD /d 60000 /f'

# Prevent "your password has expired and must be changed" after 42 days from using BVM
Execute-Command 'net accounts /maxpwage:unlimited'

# Allow Windows upgrades with unsupported TPM or CPU
Execute-Command 'reg add HKLM\SYSTEM\Setup\MoSetup /v AllowUpgradesWithUnsupportedTPMOrCPU /t REG_DWORD /d 0x00000001 /f'

# Enable Dark Mode for System UI, Apps, and wallpaper
Execute-Command 'reg add HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize /v SystemUsesLightTheme /t REG_DWORD /d 0 /f'
Execute-Command 'reg add HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize /v AppsUseLightTheme /t REG_DWORD /d 0 /f'
Execute-Command 'reg add "HKCU\Control Panel\Desktop" /v Wallpaper /t REG_SZ /d "C:\WINDOWS\web\wallpaper\Windows\img19.jpg" /f'

# Activate Windows using a generic license key from microsoft
Execute-Command 'slmgr /ipk VK7JG-NPHTM-C97JM-9MPGT-3V66T'

# Run debloat script and log output
$DebloatScript = "E:\Win11Debloat\Win11Debloat.ps1"
if (Test-Path $DebloatScript) {
    Write-Output "Executing Debloat Script..."
    powershell.exe -ExecutionPolicy Bypass -NoProfile -File $DebloatScript -RunDefaults -Silent
    if ($LASTEXITCODE -ne 0) {
        Write-Output "ERROR: Debloat script failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Output "WARNING: Debloat script not found at $DebloatScript"
}

# Everything above is for this VM only once. What follows is the setup of BVM's display modes without RDP (host folder as Z:, clipboard, automatic login, power settings,
# GPU driver). It can run again later, so it is a separate file: C:\bvm\guest-setup.cmd. The host puts it in the folder bvm of the unattended drive, if it could download
# the drivers; without it, the host does the same later with the command gpu-firstboot.
$GuestSetup = "E:\bvm\guest-setup.cmd"
if (Test-Path $GuestSetup) {
    Write-Output "Installing the BVM guest setup..."
    Execute-Command 'xcopy E:\bvm C:\bvm /e /i /y /q'
    Execute-Command 'attrib -r C:\bvm\*.* /s'
    # the launcher in the Startup folder continues the setup after a restart (the GPU driver needs one). firstboot.flag: no waiting for a key press if it fails.
    $Launcher = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\bvm-guest-setup.cmd"
    Set-Content -Path $Launcher -Encoding ASCII -Value '@echo off', 'if exist C:\bvm\guest-setup.cmd (copy /y C:\bvm\guest-setup.cmd C:\Windows\Temp\bvm-setup.cmd >nul & call C:\Windows\Temp\bvm-setup.cmd)'
    Set-Content -Path 'C:\bvm\firstboot.flag' -Value 'first boot of a new VM'
    Set-Content -Path 'C:\bvm\shutdown-when-done.flag' -Value 'shut down when the guest setup is done'
    Copy-Item 'C:\bvm\guest-setup.cmd' 'C:\Windows\Temp\bvm-setup.cmd' -Force
    # runs in this window, and shuts Windows down itself when it is done (or restarts it first)
    cmd.exe /c 'C:\Windows\Temp\bvm-setup.cmd'
    Write-Output "The guest setup returned: $LASTEXITCODE"
    # if it did not shut down by itself (it failed), do it here
    shutdown.exe -s -t 60 -c "First-run setup complete. This VM will SHUTDOWN in 60 seconds" 2>&1 | Out-Null
} else {
    # Shutdown after completion
    Execute-Command 'shutdown.exe -s -t 60 -c "First-run setup complete. This VM will SHUTDOWN in 60 seconds"'
}

# Stop logging
Stop-Transcript
