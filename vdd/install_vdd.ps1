$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$logPath = Join-Path $root 'install_log.txt'
$transcribing = $false
$exitCode = 0

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$NativeArgs
    )
    & $FilePath @NativeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Get-MivaraVddDevice {
    return @(Get-PnpDevice -HardwareId 'Root\MttVDD', 'MttVDD' -ErrorAction SilentlyContinue)
}

try {
    try {
        Start-Transcript -Path $logPath -Force -ErrorAction Stop | Out-Null
        $transcribing = $true
    } catch {
        Write-Warning "Cannot write install transcript: $($_.Exception.Message)"
    }

    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator privileges are required. Run this script from an elevated PowerShell.'
    }

    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'ARM64' } else { 'x86' }
    $drv = Join-Path $root "SignedDrivers\$arch\VDD"
    $inf = Join-Path $drv 'MttVDD.inf'
    $settingsSource = Join-Path $drv 'vdd_settings.xml'
    $devcon = Join-Path $root 'Dependencies\devcon.exe'
    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    foreach ($required in @($inf, $settingsSource, $devcon, $pnputil)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required installation file is missing: $required"
        }
    }

    Write-Output "=== 1. Add the $arch driver to the driver store ==="
    Invoke-Native $pnputil @('/add-driver', $inf, '/install')

    Write-Output '=== 2. Ensure the Root\MttVDD device exists ==='
    if ((Get-MivaraVddDevice).Count -eq 0) {
        Invoke-Native $devcon @('create', '@Root\MttVDD')
    } else {
        Write-Output 'Existing MttVDD device found; skipping device creation.'
    }

    Write-Output '=== 3. Install default settings without replacing existing ones ==='
    $settingsDir = Join-Path $env:SystemRoot 'System32\drivers\UMDF'
    $settingsTarget = Join-Path $settingsDir 'vdd_settings.xml'
    if (-not (Test-Path -LiteralPath $settingsDir -PathType Container)) {
        New-Item -ItemType Directory -Path $settingsDir -Force | Out-Null
    }
    if (-not (Test-Path -LiteralPath $settingsTarget -PathType Leaf)) {
        Copy-Item -LiteralPath $settingsSource -Destination $settingsTarget
    } else {
        Write-Output "Keeping existing settings: $settingsTarget"
    }

    Write-Output '=== 4. Scan and verify the device ==='
    Invoke-Native $pnputil @('/scan-devices')
    $device = $null
    foreach ($attempt in 1..15) {
        $device = @(Get-MivaraVddDevice)
        if ($device.Count -gt 0) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($device.Count -eq 0) {
        throw 'The driver package was processed, but no Root\MttVDD device was found.'
    }
    $device | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-String | Write-Output
    Write-Output 'Mivara virtual display driver installation completed.'
} catch {
    $exitCode = 1
    Write-Host ''
    Write-Host "VDD installation failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Installation log: $logPath"
} finally {
    if ($transcribing) {
        try { Stop-Transcript | Out-Null } catch { Write-Warning $_.Exception.Message }
    }
}

exit $exitCode
