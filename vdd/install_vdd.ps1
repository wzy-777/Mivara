param(
    [switch]$Reinstall
)

$ErrorActionPreference = 'Stop'

# Inno Setup is a 32-bit process, so the installer (and any 32-bit shell) starts
# this script with 32-bit PowerShell. There %SystemRoot%\System32 is redirected
# to SysWOW64, where pnputil.exe does not exist, and the driver store cannot be
# written correctly. Re-launch under 64-bit PowerShell through Sysnative (the
# alias that is only visible to 32-bit processes) and forward the exit code.
if (-not [Environment]::Is64BitProcess) {
    $ps64 = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $ps64 -PathType Leaf)) {
        Write-Host 'VDD installation failed: 64-bit PowerShell is required.' -ForegroundColor Red
        exit 1
    }
    $forward = @()
    if ($Reinstall) { $forward += '-Reinstall' }
    & $ps64 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath @forward
    exit $LASTEXITCODE
}

$root = $PSScriptRoot
$logPath = Join-Path $root 'install_log.txt'
$transcribing = $false
$exitCode = 0

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$NativeArgs,
        [int[]]$AllowedExitCodes = @(0)
    )
    & $FilePath @NativeArgs
    if ($AllowedExitCodes -notcontains $LASTEXITCODE) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Get-MivaraVddDevice {
    return @(Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -contains 'Root\MttVDD' -or $_.HardwareID -contains 'MttVDD' })
}

function Install-DefaultSettings {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Directory
    )
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    }
    $target = Join-Path $Directory 'vdd_settings.xml'
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        Copy-Item -LiteralPath $Source -Destination $target
    } else {
        Write-Output "Keeping existing settings: $target"
    }
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
    $settingsDir = Join-Path $env:SystemRoot 'System32\drivers\UMDF'
    foreach ($required in @($inf, $settingsSource, $devcon, $pnputil)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required installation file is missing: $required"
        }
    }

    # A present device means the driver is already installed: leave it alone
    # unless the installer asked for a reinstall (task "reinstallvdd") or the
    # device reports a failed driver load. Re-running the whole install on an
    # up-to-date device is what made pnputil exit with 259 before.
    $devices = @(Get-MivaraVddDevice)
    $broken = @($devices | Where-Object {
        $_.Problem -eq 'CM_PROB_FAILED_INSTALL' -or $_.Problem -eq 'CM_PROB_FAILED_POST_START'
    })
    if ($devices.Count -gt 0 -and -not $Reinstall -and $broken.Count -eq 0) {
        Write-Output '=== MttVDD is already installed; leaving the driver and device untouched ==='
        Write-Output 'Select "Reinstall virtual display driver" in the installer to force a reinstall.'
        Install-DefaultSettings -Source $settingsSource -Directory $settingsDir
        $devices | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-String | Write-Output
        return
    }

    Write-Output "=== 1. Add the $arch driver package to the driver store ==="
    # Import only (no /install).  With /install, pnputil returns 259 when no
    # device was updated -- either because the device does not exist yet or
    # because it is already on the newest driver -- which is not a failure.
    Invoke-Native -FilePath $pnputil -NativeArgs @('/add-driver', $inf) -AllowedExitCodes @(0, 259)

    Write-Output '=== 2. Create or update the Root\MttVDD device ==='
    if ($devices.Count -eq 0) {
        Invoke-Native -FilePath $devcon -NativeArgs @('install', $inf, 'Root\MttVDD')
    } else {
        Write-Output 'Reinstalling the driver on the existing MttVDD device.'
        Invoke-Native -FilePath $devcon -NativeArgs @('update', $inf, 'Root\MttVDD')
    }

    Write-Output '=== 3. Install default settings without replacing existing ones ==='
    Install-DefaultSettings -Source $settingsSource -Directory $settingsDir

    Write-Output '=== 4. Scan and verify the device ==='
    Invoke-Native -FilePath $pnputil -NativeArgs @('/scan-devices') -AllowedExitCodes @(0, 259)
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
