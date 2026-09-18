# Restart the host helper so code changes take effect (keeps the Startup shortcut).
# Usage:  .\update_helper.ps1
#         .\update_helper.ps1 -Deps    # also refresh Python packages
#
# First-time setup is still:  .\install_startup.ps1
# Full remove:                 .\uninstall_startup.ps1

param(
    [switch]$Deps
)

$ErrorActionPreference = "Stop"

$hostDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $hostDir "host_sync.py"
$reqs = Join-Path $hostDir "requirements.txt"
$startup = [Environment]::GetFolderPath("Startup")
$lnkPath = Join-Path $startup "Macro Keyboard Volume.lnk"

if (-not (Test-Path $script)) {
    throw "host_sync.py not found next to this script."
}

function Get-PythonW {
    $cmd = Get-Command pythonw -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        $candidate = Join-Path (Split-Path $py.Source) "pythonw.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    return $null
}

function Stop-HostSync {
    $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*host_sync.py*" }
    foreach ($p in $procs) {
        Write-Host "Stopping PID $($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if (-not $procs) {
        Write-Host "Helper was not running."
    }
}

$pythonw = Get-PythonW
if (-not $pythonw) {
    throw "pythonw.exe not found. Install Python with 'pythonw' on PATH."
}

Stop-HostSync
Start-Sleep -Milliseconds 400

if ($Deps) {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        throw "python.exe not found (needed for -Deps)."
    }
    if (-not (Test-Path $reqs)) {
        throw "requirements.txt not found."
    }
    Write-Host "Updating packages from requirements.txt ..."
    & $python.Source -m pip install -r $reqs
}

# Keep / refresh the login Startup shortcut without a full reinstall dance.
$wsh = New-Object -ComObject WScript.Shell
$lnk = $wsh.CreateShortcut($lnkPath)
$lnk.TargetPath = $pythonw
$lnk.Arguments = "`"$script`""
$lnk.WorkingDirectory = $hostDir
$lnk.WindowStyle = 7
$lnk.Description = "Pushes Windows volume + now-playing to the macro keyboard OLED"
$lnk.Save()

Start-Process -FilePath $pythonw -ArgumentList "`"$script`"" -WorkingDirectory $hostDir -WindowStyle Hidden
Write-Host "Helper restarted."
Write-Host "Startup shortcut OK: $lnkPath"
