param(
    [string]$ComPort = "COM3",
    [int]$BaudRate = 115200,
    [int]$WaitSeconds = 15
)

if ($global:port -is [System.IO.Ports.SerialPort] -and $global:port.IsOpen) {
    $global:port.Close()
}

function Get-AvailableComPorts {
    [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
}

$port = New-Object System.IO.Ports.SerialPort $ComPort, $BaudRate, None, 8, one
$port.ReadTimeout = 200
$port.WriteTimeout = 200
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.DtrEnable = $true
$port.RtsEnable = $true

$opened = $false
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while (-not $opened) {
    try {
        $port.Open()
        $opened = $true
    } catch {
        if ((Get-Date) -ge $deadline) {
            $available = @(Get-AvailableComPorts)
            Write-Host "Could not open $ComPort : $($_.Exception.Message)"
            if ($available.Count -eq 0) {
                Write-Host "No COM ports found. The Pico is probably in BOOTSEL, still rebooting after flash, or unplugged."
            } else {
                Write-Host ("Ports right now: " + ($available -join ", "))
                Write-Host "If yours moved, run:  .\serial_monitor.ps1 -ComPort COMx"
            }
            exit 1
        }
        Write-Host "Waiting for $ComPort ..."
        Start-Sleep -Milliseconds 500
    }
}

Start-Sleep -Milliseconds 300

Write-Host "Listening on $ComPort. Press a keyboard button. Ctrl+C to stop."

$buffer = New-Object byte[] 256
try {
    while ($true) {
        try {
            $n = $port.Read($buffer, 0, $buffer.Length)
            if ($n -gt 0) {
                Write-Host -NoNewline ([System.Text.Encoding]::ASCII.GetString($buffer, 0, $n))
            }
        } catch [System.TimeoutException] {
        } catch {
            Write-Host $_.Exception.Message
            break
        }
    }
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
}
