[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port = 'COM8',
    [ValidateRange(9600, 2000000)][int]$Baud = 460800,
    [switch]$BuildOnly,
    [switch]$Monitor,
    [string]$ActivationScript = '',
    [string]$InstallRegistry = 'C:\Espressif\tools\eim_idf.json'
)
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
# Some agent-launched shells omit this Windows variable; Python platform.machine
# then returns an empty string and IDF cannot select its toolchain.
if (-not $env:PROCESSOR_ARCHITECTURE) {
    $osArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $env:PROCESSOR_ARCHITECTURE = switch ($osArch) {
        'X64' { 'AMD64' }
        'Arm64' { 'ARM64' }
        'X86' { 'x86' }
        default { throw "Unsupported OS architecture: $osArch" }
    }
}
# Native exit codes are checked explicitly, including on PowerShell 7.
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

if (-not $ActivationScript -and -not $env:IDF_PATH) {
    if (-not (Test-Path -LiteralPath $InstallRegistry)) {
        throw 'ESP-IDF not found. Run in an ESP-IDF terminal or supply -ActivationScript.'
    }
    $registry = Get-Content -Raw -LiteralPath $InstallRegistry | ConvertFrom-Json
    $installations = @($registry.idfInstalled | Where-Object {
        $_.activationScript -and (Test-Path -LiteralPath $_.activationScript)
    })
    $chosen = $installations | Where-Object id -EQ $registry.idfSelectedId | Select-Object -First 1
    if (-not $chosen -and $installations.Count -eq 1) { $chosen = $installations[0] }
    if (-not $chosen) { throw 'Multiple/no ESP-IDF installations. Supply -ActivationScript explicitly.' }
    $ActivationScript = $chosen.activationScript
}
if ($ActivationScript) {
    if (-not (Test-Path -LiteralPath $ActivationScript)) { throw "Not found: $ActivationScript" }
    # EIM profile supports -e: print environment without interactive shell setup.
    Write-Host "Loading ESP-IDF environment: $ActivationScript"
    $savedPath = $env:PATH
    $pairs = & $ActivationScript -e 6>&1
    foreach ($entry in $pairs) {
        $line = $entry.ToString()
        if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
            $key = $Matches[1]
            $value = $Matches[2]
            if ($key -eq 'SYSTEM_PATH') { continue }
            if ($key -eq 'PATH') { $value = "$value;$savedPath" }
            [Environment]::SetEnvironmentVariable($key, $value, 'Process')
        }
    }
}
$idfScript = Join-Path $env:IDF_PATH 'tools\idf.py'
if (-not (Test-Path -LiteralPath $idfScript)) { throw "idf.py not found: $idfScript" }
$pythonExe = if ($env:IDF_PYTHON_ENV_PATH) {
    Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
} else { (Get-Command python -ErrorAction Stop).Source }
if (-not (Test-Path -LiteralPath $pythonExe)) { throw "Python not found: $pythonExe" }
function Invoke-IdfChecked {
    param([string[]]$IdfArgs)
    & $pythonExe $idfScript @IdfArgs
    if ($LASTEXITCODE -ne 0) { throw "idf.py failed (exit $LASTEXITCODE): $($IdfArgs -join ' ')" }
}

Push-Location $PSScriptRoot
try {
    # Do not delete or reset an existing build/configuration automatically.
    if (Test-Path -LiteralPath 'sdkconfig') {
        $target = Select-String -LiteralPath 'sdkconfig' -Pattern '^CONFIG_IDF_TARGET="([^"]+)"'
        if ($target -and $target.Matches[0].Groups[1].Value -ne 'esp32s3') {
            throw 'Existing sdkconfig targets another chip; use a separate build/configuration.'
        }
    }
    Invoke-IdfChecked -IdfArgs @('--version')
    Invoke-IdfChecked -IdfArgs @('-DIDF_TARGET=esp32s3', 'build')
    if (-not $BuildOnly) {
        Write-Host "Flashing HP2300 ESP-IDF printer sender to $Port at $Baud baud."
        Invoke-IdfChecked -IdfArgs @('-p', $Port, '-b', "$Baud", 'flash')
        Write-Host "Flash completed: $Port"
        if ($Monitor) { Invoke-IdfChecked -IdfArgs @('-p', $Port, 'monitor', '--no-reset') }
    } else { Write-Host 'Build completed; nothing flashed.' }
} finally { Pop-Location }
