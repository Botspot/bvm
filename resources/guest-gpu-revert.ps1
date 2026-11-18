# BVM GPU mode, for DEBUGGING: undo what gpu-firstboot did inside Windows, so the next gpu-firstboot is a clean run. Nothing injects or runs this by itself.
#
# Run it INSIDE Windows (an administrator prompt: in a BVM VM every prompt is, UAC is off), with the VM started by boot-gpu, so that the host's folder is drive Z:
# (the shared_folder of bvm-config, your home folder by default; adjust the path if BVM is not in ~/bvm). It copies itself to %TEMP% first, because it removes the
# Z: settings while it runs. One line for a command prompt (the first command maps Z: if it is not there):
#
#   net use Z: \\10.0.2.4\qemu & copy /y Z:\bvm\resources\guest-gpu-revert.ps1 %TEMP% && powershell -ExecutionPolicy Bypass -File %TEMP%\guest-gpu-revert.ps1 -Shutdown
#
# and the same from PowerShell:
#
#   net use Z: \\10.0.2.4\qemu; Copy-Item Z:\bvm\resources\guest-gpu-revert.ps1 $env:TEMP -Force; powershell -ExecutionPolicy Bypass -File $env:TEMP\guest-gpu-revert.ps1 -Shutdown
#
# Removes: the logon/startup entries and the files of the guest setup (C:\bvm, its state in HKLM:\SOFTWARE\BVM-GPU, the Group Policy startup entry), the Triton
# GPU driver (Windows falls back to the stock viogpudo driver of BVM), the SPICE agent, the Z: mapping and the SMB client settings, the power plan and update
# tweaks. Leaves what a stock BVM install has: the VirtIO serial and display drivers, qemu-ga, the automatic login (it is the one BVM set up), hibernation off.
# On the host nothing changes: afterwards run  bvm gpu-firstboot DIR  (it always boots Windows and runs the setup again). The GUI skips that step while
# DIR/gpu/state says transitioned, so after a revert run gpu-firstboot by hand. Safe to run twice. Log: C:\Windows\Temp\bvm-gpu-revert.log
param([switch]$Shutdown)   # -Shutdown: shut Windows down 20 seconds after the last line (cancel with: shutdown /a)
$ErrorActionPreference = 'Continue'
$Log = 'C:\Windows\Temp\bvm-gpu-revert.log'
function Log([string]$m) { $l = '{0:HH:mm:ss} {1}' -f (Get-Date), $m; Add-Content $Log $l; Write-Host $l }
function Native([string]$c) { $o = (& cmd.exe /c "$c 2>&1") -join "`n"; $script:rc = $LASTEXITCODE; $o }
function Section([string]$name, [scriptblock]$sb) { Log "== $name"; try { & $sb } catch { Log "   FAILED: $($_.Exception.Message)" } }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Run this from an administrator prompt.'; exit 1
}
Log "revert start ($env:COMPUTERNAME, user $env:USERNAME)"
$RunKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'

Section 'startup and logon entries of the guest setup' {
    # the local Group Policy startup entry for C:\bvm\guest-autologon.cmd (or guest-setup.ps1 of the PowerShell versions); other entries in the file are kept
    $ini = 'C:\Windows\System32\GroupPolicy\Machine\Scripts\scripts.ini'
    if (Test-Path $ini) {
        $l = @(Get-Content $ini); $keep = @()
        for ($i = 0; $i -lt $l.Count; $i++) {
            if ($l[$i] -match '^\d+CmdLine=' -and ($i + 1) -lt $l.Count -and $l[$i + 1] -match 'guest-setup\.ps1|guest-autologon\.cmd') { $i++; continue }
            $keep += $l[$i]
        }
        if ($keep.Count -ne $l.Count) {
            if (@($keep | Where-Object { $_ -match '^\d+CmdLine=' }).Count) { Set-Content -Path $ini -Value $keep -Encoding Unicode } else { Remove-Item -Force $ini }
            Log '   Group Policy startup entry removed'
        }
    }
    foreach ($n in 'BVM-MapShare', 'BVM-GuestProgress') {
        if (Get-ItemProperty $RunKey -Name $n -ErrorAction SilentlyContinue) { Remove-ItemProperty $RunKey -Name $n -ErrorAction SilentlyContinue; Log "   Run entry $n removed" }
    }
    # a launcher from earlier versions
    $su = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\bvm-guest-setup.cmd"
    if (Test-Path $su) { Remove-Item -Force $su; Log '   Startup folder launcher removed' }
    # a progress window or a setup that is running right now
    Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe' OR Name = 'cmd.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'guest-setup\.(ps1|cmd)|guest-progress|bvm-progress|bvm-setup' -and $_.ProcessId -ne $PID } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; Log "   stopped process $($_.ProcessId)" }
    Remove-Item -Force 'C:\Windows\Temp\bvm-setup.cmd', 'C:\Windows\Temp\bvm-setup.lock', 'C:\Windows\Temp\bvm-progress.cmd', 'C:\Windows\Temp\bvm-progress.lock' -ErrorAction SilentlyContinue
}

Section 'Triton (viogpu3d) GPU driver -> stock viogpudo' {
    $pk = @(Get-WindowsDriver -Online -All | Where-Object { $_.OriginalFileName -like '*\viogpu3d.inf' })
    if (-not $pk) { Log '   no viogpu3d driver package installed' }
    foreach ($d in $pk) { Log "   removing $($d.Driver): $(Native "pnputil /delete-driver $($d.Driver) /uninstall /force")" }
    $null = Native 'pnputil /scan-devices'
}

Section 'SPICE guest agent' {
    if (Get-Service vdservice -ErrorAction SilentlyContinue) { Stop-Service vdservice -Force -ErrorAction SilentlyContinue; $null = Native 'sc delete vdservice'; Log '   vdservice removed' }
    Get-Process -Name vdagent -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item -Recurse -Force 'C:\Program Files\SPICE Agent' -ErrorAction SilentlyContinue
}

Section 'SMB client settings and Z: drive' {
    $null = Native 'net use Z: /delete /y'
    Remove-Item 'HKCU:\Network\Z' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters' -Name AllowInsecureGuestAuth -ErrorAction SilentlyContinue
    # Windows 11 24H2 (build 26100+) requires SMB signing on the client by default, 23H2 does not
    $req = ([Environment]::OSVersion.Version.Build -ge 26100)
    Set-SmbClientConfiguration -EnableInsecureGuestLogons $false -RequireSecuritySignature $req -Force -Confirm:$false
    Log "   guest logons off, RequireSecuritySignature=$req"
}

Section 'power plan' {
    $null = Native 'powercfg -restoredefaultschemes'
    $null = Native 'powercfg -h off'        # BVM turns hibernation off itself
    Remove-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -ErrorAction SilentlyContinue
}

Section 'Windows Update tweaks (the optional update-hygiene step)' {
    $t = Get-ScheduledTask -TaskPath '\Microsoft\Windows\PI\' -TaskName 'Secure-Boot-Update' -ErrorAction SilentlyContinue
    if ($t -and $t.State -eq 'Disabled') { Enable-ScheduledTask -InputObject $t | Out-Null; Log '   Secure-Boot-Update task enabled' }
    try { Remove-MpPreference -ExclusionPath 'C:\Windows\SoftwareDistribution','C:\Windows\WinSxS','C:\Windows\servicing' -ErrorAction Stop
          Remove-MpPreference -ExclusionProcess 'TiWorker.exe','TrustedInstaller.exe' -ErrorAction Stop } catch { }
}

Section 'state and files of the guest setup' {
    Remove-Item -Recurse -Force 'HKLM:\SOFTWARE\BVM-GPU' -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force 'C:\bvm' -ErrorAction SilentlyContinue
    if (Test-Path 'C:\bvm') { Log '   C:\bvm could not be removed completely (files in use?), delete it by hand after the restart' } else { Log '   C:\bvm removed (with the setup log)' }
}

Section 'result' {
    Log ('   viogpu3d packages left: ' + @(Get-WindowsDriver -Online -All | Where-Object { $_.OriginalFileName -like '*\viogpu3d.inf' }).Count)
    Log ('   viogpudo packages: ' + @(Get-WindowsDriver -Online -All | Where-Object { $_.OriginalFileName -like '*\viogpudo.inf' }).Count)
    Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object { Log "   display: $($_.FriendlyName) [$($_.Status)]" }
}
Log 'done. Shut Windows down (a restart is not enough) and run bvm gpu-firstboot on the host to repeat the setup from a clean state.'
if ($Shutdown) { Log 'shutting down in 20 s (cancel: shutdown /a)'; $null = Native 'shutdown /s /t 20 /c "BVM GPU revert finished"' }
