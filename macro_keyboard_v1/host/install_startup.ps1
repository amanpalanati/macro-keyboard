# Create a hidden Startup shortcut so host_sync.py runs at login (no console).
# Usage:  .\install_startup.ps1
# Remove: .\uninstall_startup.ps1

$ErrorActionPreference = "Stop"

$hostDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $hostDir "host_sync.py"
$startup = [Environment]::GetFolderPath("Startup")
$lnkPath = Join-Path $startup "Macro Keyboard Volume.lnk"
$oldLnkPath = Join-Path $startup "Macro Keypad Volume.lnk"
if (Test-Path $oldLnkPath) {
    Remove-Item $oldLnkPath -Force
}

if (-not (Test-Path $script)) {
    throw "host_sync.py not found next to this installer."
}

$pythonw = $null
$cmd = Get-Command pythonw -ErrorAction SilentlyContinue
if ($cmd) {
    $pythonw = $cmd.Source
} else {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        $candidate = Join-Path (Split-Path $py.Source) "pythonw.exe"
        if (Test-Path $candidate) {
            $pythonw = $candidate
        }
    }
}

if (-not $pythonw) {
    throw "pythonw.exe not found. Install Python with the py launcher / 'pythonw' on PATH."
}

$wsh = New-Object -ComObject WScript.Shell
$lnk = $wsh.CreateShortcut($lnkPath)
$lnk.TargetPath = $pythonw
$lnk.Arguments = "`"$script`""
$lnk.WorkingDirectory = $hostDir
$lnk.WindowStyle = 7
$lnk.Description = "Pushes Windows volume to the macro keyboard OLED"
$lnk.Save()

$already = Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -like "*host_sync.py*" }

if (-not $already) {
    Start-Process -FilePath $pythonw -ArgumentList "`"$script`"" -WorkingDirectory $hostDir -WindowStyle Hidden
    Write-Host "Started in the background."
} else {
    Write-Host "Already running."
}

Write-Host "Installed: $lnkPath"
Write-Host "It will start with Windows. Unplug/replug the keyboard anytime; the helper waits for it."
Write-Host "Remove with: .\uninstall_startup.ps1"
