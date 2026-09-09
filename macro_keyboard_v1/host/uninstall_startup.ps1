# Remove the Startup shortcut and stop a running host_sync.py helper.
# Usage:  .\uninstall_startup.ps1

$ErrorActionPreference = "Stop"

$startup = [Environment]::GetFolderPath("Startup")
$lnkNames = @("Macro Keyboard Volume.lnk", "Macro Keypad Volume.lnk")
$removed = $false
foreach ($name in $lnkNames) {
    $lnkPath = Join-Path $startup $name
    if (Test-Path $lnkPath) {
        Remove-Item $lnkPath -Force
        Write-Host "Removed $lnkPath"
        $removed = $true
    }
}
if (-not $removed) {
    Write-Host "No Startup shortcut found."
}

Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -like "*host_sync.py*" } |
    ForEach-Object {
        Write-Host "Stopping PID $($_.ProcessId)"
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
