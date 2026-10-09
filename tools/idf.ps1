# ESP-IDF environment bootstrap for this workspace.
#
# Why this file exists:
#   To start idf.py with an interpreter that has the ESP-IDF requirements, and
#   to always pick the same one. idf.py records the interpreter path inside
#   build/CMakeCache.txt and stops with a message asking for "fullclean" if a
#   different interpreter shows up afterwards.
#
#   Two venvs exist on this machine:
#     - C:\Espressif\tools\python\v6.1\venv   created by the ESP-IDF installer
#     - D:\ESP32P4C5\.tools\idf_py_venv       workspace-local fallback, built
#       from the PlatformIO Python 3.11 interpreter
#
# Usage:
#   . D:\ESP32P4C5\tools\idf.ps1
#   idf build
#   idf -C D:\ESP32P4C5\ESP32P4\ESP32-P4 build

$ProjectDir   = 'D:\ESP32P4C5\ESP32P4\ESP32-P4'
$IdfRoot      = 'D:\ESP-IDF\.espressif\v6.1\esp-idf'
$ToolsRoot    = 'C:\Espressif'
$IdfVersion   = '6.1'

# Keep this file ASCII-only, or save it as UTF-8 *with* a BOM.
# Windows PowerShell 5.1 reads a script without a BOM as ANSI, so non-ASCII
# text is mis-decoded and a multi-byte character can swallow the newline after
# it. That silently comments out the following line of code. It is exactly how
# the "$IdfArgs = $args" line in the idf function below once disappeared, which
# made every "idf ..." call arrive at idf.py with no arguments at all.

# Which Python runs idf.py. The interpreter recorded in build/CMakeCache.txt is
# tried first: reusing it avoids the virtualenv mismatch that makes idf.py
# print its help text and exit with code 2 (which looks like a bad-argument
# error but is not).
$PythonEnvCandidates = New-Object System.Collections.Generic.List[string]
$cmakeCache = Join-Path $ProjectDir 'build\CMakeCache.txt'
if (Test-Path $cmakeCache) {
    $configured = Select-String -Path $cmakeCache -Pattern '^PYTHON:[^=]*=(.+)$' | Select-Object -First 1
    if ($configured) {
        # C:\...\venv\Scripts\python.exe  ->  C:\...\venv
        $exe = $configured.Matches[0].Groups[1].Value.Trim()
        $PythonEnvCandidates.Add((Split-Path (Split-Path $exe -Parent) -Parent))
    }
}
$PythonEnvCandidates.Add('C:\Espressif\tools\python\v6.1\venv')
$PythonEnvCandidates.Add('D:\ESP32P4C5\.tools\idf_py_venv')

$PythonEnv = $null
foreach ($candidate in $PythonEnvCandidates) {
    $exe = Join-Path $candidate 'Scripts\python.exe'
    if (-not (Test-Path $exe)) { continue }
    & $exe -c 'import click, kconfiglib, esptool' *> $null
    if ($LASTEXITCODE -eq 0) { $PythonEnv = $candidate; break }
}
if (-not $PythonEnv) {
    Write-Host 'No usable ESP-IDF Python virtualenv found. Checked:' -ForegroundColor Red
    $PythonEnvCandidates | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    return
}

$env:IDF_PATH                   = $IdfRoot
$env:IDF_TOOLS_PATH             = $ToolsRoot
$env:IDF_PYTHON_ENV_PATH        = $PythonEnv
$env:ESP_IDF_VERSION            = $IdfVersion
$env:IDF_PYTHON_CHECK_CONSTRAINTS = '0'
$env:PYTHONUTF8                 = '1'
$env:IDF_CCACHE_ENABLE          = '1'
$env:ESP_ROM_ELF_DIR            = "$ToolsRoot\tools\esp-rom-elfs\20241011"

# ccache defaults to %LOCALAPPDATA%\ccache, which is outside this workspace and
# therefore not writable by the build sandbox ("ccache: error: Permission
# denied"). Keep the cache inside the workspace instead.
$env:CCACHE_DIR                 = 'D:\ESP32P4C5\.tools\ccache'
if (-not (Test-Path $env:CCACHE_DIR)) { New-Item -ItemType Directory -Force -Path $env:CCACHE_DIR | Out-Null }

# The build runs under a sandbox account that does not own the ESP-IDF
# checkout; tell git to trust it without touching the user's global config.
$env:GIT_CONFIG_COUNT           = '3'
$env:GIT_CONFIG_KEY_0           = 'safe.directory'
$env:GIT_CONFIG_VALUE_0         = $IdfRoot
$env:GIT_CONFIG_KEY_1           = 'safe.directory'
$env:GIT_CONFIG_VALUE_1         = "$IdfRoot/components/openthread/openthread"
$env:GIT_CONFIG_KEY_2           = 'safe.directory'
$env:GIT_CONFIG_VALUE_2         = "$IdfRoot/components/mbedtls/mbedtls"

$script:ToolPaths = @(
    "$PythonEnv\Scripts"
    "$ToolsRoot\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin"
    "$ToolsRoot\tools\cmake\4.0.3\bin"
    "$ToolsRoot\tools\ninja\1.12.1"
    "$ToolsRoot\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"
    "$ToolsRoot\tools\esp-rom-elfs\20241011"
    "$ToolsRoot\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin"
)

foreach ($p in $script:ToolPaths) {
    if ((Test-Path $p) -and ($env:PATH -notlike "*$p*")) {
        $env:PATH = "$p;$env:PATH"
    }
}

# The sandbox this workspace runs in does not hand the shell's PATH to child
# python processes; every python.exe gets the PATH captured when the shell was
# created. So idf.py is launched with an explicitly built environment instead
# of relying on the shell's PATH. Same reason ccache/ninja are not discovered
# by CMake when idf.py is started the naive way.
function global:idf {
    # 注意:这里刻意不写 param()/[CmdletBinding()]。只要带上 [Parameter()] 或
    # [CmdletBinding()],PowerShell 就把它当成高级函数并注入通用参数,
    # `idf -p COM6 flash` 里的 -p 会被拿去匹配 -ProgressAction 之类,报
    # "parameter name 'p' is ambiguous"。用简单函数 + $args 才能原样透传。
    $IdfArgs = $args

    $python = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
    $idfPy  = "$env:IDF_PATH\tools\idf.py"
    $interactive = $IdfArgs -contains 'monitor'

    $childPath = (($script:ToolPaths + ($env:PATH -split ';')) |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique) -join ';'

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName  = $python
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.EnvironmentVariables['PATH']                   = $childPath
    $psi.EnvironmentVariables['IDF_PATH']               = $env:IDF_PATH
    $psi.EnvironmentVariables['IDF_TOOLS_PATH']         = $env:IDF_TOOLS_PATH
    $psi.EnvironmentVariables['IDF_PYTHON_ENV_PATH']    = $env:IDF_PYTHON_ENV_PATH
    $psi.EnvironmentVariables['ESP_IDF_VERSION']        = $env:ESP_IDF_VERSION
    $psi.EnvironmentVariables['IDF_PYTHON_CHECK_CONSTRAINTS'] = $env:IDF_PYTHON_CHECK_CONSTRAINTS
    $psi.EnvironmentVariables['PYTHONUTF8']             = $env:PYTHONUTF8
    $psi.EnvironmentVariables['IDF_CCACHE_ENABLE']      = $env:IDF_CCACHE_ENABLE
    $psi.EnvironmentVariables['ESP_ROM_ELF_DIR']        = $env:ESP_ROM_ELF_DIR
    $psi.EnvironmentVariables['CCACHE_DIR']             = $env:CCACHE_DIR
    for ($i = 0; $i -lt [int]$env:GIT_CONFIG_COUNT; $i++) {
        $psi.EnvironmentVariables["GIT_CONFIG_KEY_$i"]   = [System.Environment]::GetEnvironmentVariable("GIT_CONFIG_KEY_$i")
        $psi.EnvironmentVariables["GIT_CONFIG_VALUE_$i"] = [System.Environment]::GetEnvironmentVariable("GIT_CONFIG_VALUE_$i")
    }
    $psi.EnvironmentVariables['GIT_CONFIG_COUNT']       = $env:GIT_CONFIG_COUNT

    $quoted = @("`"$idfPy`"")
    foreach ($a in $IdfArgs) {
        $quoted += if ($a -match '\s') { "`"$a`"" } else { $a }
    }
    $psi.Arguments = $quoted -join ' '

    if ($interactive) {
        # monitor needs a real console (stdin + Ctrl+] handling).
        $proc = [System.Diagnostics.Process]::Start($psi)
        $proc.WaitForExit()
    } else {
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError  = $true
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $psi.StandardErrorEncoding  = [System.Text.Encoding]::UTF8
        $proc = [System.Diagnostics.Process]::Start($psi)
        $out = $proc.StandardOutput.ReadToEndAsync()
        $err = $proc.StandardError.ReadToEndAsync()
        $proc.WaitForExit()
        Write-Host $out.Result
        if ($err.Result) { Write-Host $err.Result -ForegroundColor DarkYellow }
    }

    $global:LASTEXITCODE = $proc.ExitCode
    return $proc.ExitCode
}

Write-Host "ESP-IDF $IdfVersion ready" -ForegroundColor Green
Write-Host "  IDF_PATH            = $env:IDF_PATH"
Write-Host "  IDF_PYTHON_ENV_PATH = $env:IDF_PYTHON_ENV_PATH"
Write-Host "  python              = $env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
Write-Host "Use: idf build | idf flash | idf monitor"
