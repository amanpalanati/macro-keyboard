param(
    [string]$ComPort = "COM3",
    [int]$BaudRate = 115200
)

if ($global:port -is [System.IO.Ports.SerialPort] -and $global:port.IsOpen) {
    $global:port.Close()
}

$port = New-Object System.IO.Ports.SerialPort $ComPort, $BaudRate, None, 8, one
$port.ReadTimeout = 200
$port.WriteTimeout = 200
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.DtrEnable = $true
$port.RtsEnable = $true

try {
    $port.Open()
} catch {
    Write-Host "Could not open $ComPort : $($_.Exception.Message)"
    Write-Host "Close FancyMon or any other serial monitor, then try again."
    exit 1
}

Start-Sleep -Milliseconds 300

Write-Host "Listening on $ComPort. Press a keypad button. Ctrl+C to stop."

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
