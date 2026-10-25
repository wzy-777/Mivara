$installer = Join-Path $PSScriptRoot 'install_vdd.ps1'
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    Write-Error "Cannot find the unified VDD installer: $installer"
    exit 1
}

& $installer @args
if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE }
exit 0
