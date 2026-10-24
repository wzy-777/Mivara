$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
Start-Transcript -Path (Join-Path $root 'install_log2.txt') -Force
$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'ARM64' } else { 'x86' }
$drv = Join-Path $root "SignedDrivers\$arch\VDD"
Set-Location $drv
Write-Output "=== 1. devcon install (creates Root\MttVDD device) ==="
& (Join-Path $root 'Dependencies\devcon.exe') install MttVDD.inf "Root\MttVDD"
Write-Output "=== 2. Copy settings next to driver DLL ==="
Copy-Item "$drv\vdd_settings.xml" 'C:\Windows\System32\drivers\UMDF\vdd_settings.xml' -Force
Write-Output "=== 3. wait + rescan ==="
Start-Sleep -Seconds 3
pnputil /scan-devices
Start-Sleep -Seconds 3
Write-Output "=== 4. Monitor devices ==="
pnputil /enum-devices /class Monitor
Write-Output "=== 5. WMI monitors ==="
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue | ForEach-Object {
    $name = ($_.UserFriendlyName | Where-Object {$_ -ne 0} | ForEach-Object {[char]$_}) -join ''
    Write-Output ("Monitor: " + $name + "  InstanceName=" + $_.InstanceName)
}
Stop-Transcript
