$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
Start-Transcript -Path (Join-Path $root 'install_log2.txt') -Force
$drv = Join-Path $root 'SignedDrivers\x86\VDD'
Set-Location $drv
Write-Output "=== 1. devcon install (creates Root\MttVDD device) ==="
& (Join-Path $root 'Dependencies\devcon.exe') install MttVDD.inf "Root\MttVDD"
Write-Output "=== 2. wait + rescan ==="
Start-Sleep -Seconds 3
pnputil /scan-devices
Start-Sleep -Seconds 3
Write-Output "=== 3. Monitor devices ==="
pnputil /enum-devices /class Monitor
Write-Output "=== 4. WMI monitors ==="
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue | ForEach-Object {
    $name = ($_.UserFriendlyName | Where-Object {$_ -ne 0} | ForEach-Object {[char]$_}) -join ''
    Write-Output ("Monitor: " + $name + "  InstanceName=" + $_.InstanceName)
}
Stop-Transcript
