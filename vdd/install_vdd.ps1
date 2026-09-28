$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
Start-Transcript -Path (Join-Path $root 'install_log.txt') -Force
$drv = Join-Path $root 'SignedDrivers\x86\VDD'
Set-Location $drv
Write-Output "=== 1. Add driver to driver store ==="
pnputil /add-driver MttVDD.inf /install
Write-Output "=== 2. Create root device Root\MttVDD ==="
& (Join-Path $root 'Dependencies\devcon.exe') create "@Root\MttVDD"
Write-Output "=== 3. Copy settings next to driver DLL ==="
Copy-Item "$drv\vdd_settings.xml" 'C:\Windows\System32\drivers\UMDF\vdd_settings.xml' -Force
Write-Output "=== 4. Scan devices ==="
pnputil /scan-devices
Start-Sleep -Seconds 3
Write-Output "=== 5. Current monitors ==="
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue | ForEach-Object {
    $name = ($_.UserFriendlyName | Where-Object {$_ -ne 0} | ForEach-Object {[char]$_}) -join ''
    Write-Output ("Monitor: " + $name + "  InstanceName=" + $_.InstanceName)
}
Stop-Transcript
