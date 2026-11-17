# Run the Windows Server product checks under the runner account, with a
# process lifetime independent of the Actions step's Job Object.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $BuildDir,
    [Parameter(Mandatory)][string] $Config,
    [string] $DeployDir,
    [string] $Python,
    [string] $CMake,
    [string] $CTest,
    [string] $Transcript,
    [string] $ExpectedSid
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if ($DeployDir) {
    $DeployDir = (Resolve-Path -LiteralPath $DeployDir).Path
}
$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value

function Invoke-Check([string] $Program, [string[]] $Arguments) {
    & $Program @Arguments 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$Program $($Arguments -join ' ') exited with code $LASTEXITCODE"
    }
}

if ($Transcript) {
    Start-Transcript -Path $Transcript | Out-Null
    $status = 1
    try {
        if ($sid -ne $ExpectedSid) {
            throw 'Windows MCP tests must run as the original runner account'
        }
        $env:PYTHONUTF8 = '1'
        $env:Path = @((Split-Path -Parent $CMake), (Split-Path -Parent $CTest), $env:Path) -join ';'
        Write-Host "Windows MCP checks on $([Environment]::OSVersion.Version), $env:PROCESSOR_ARCHITECTURE, SID $sid"
        if ($DeployDir) {
            Invoke-Check $CTest @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure')
            $relocation = @('scripts/check_mcp_install.py', '--build-dir', $BuildDir, '--config', $Config, '--mcp', 'ON')
            Invoke-Check $Python $relocation
            Invoke-Check $Python ($relocation + @('--package-dir', $DeployDir, '--package-executable', 'hdmapping_mcp.exe'))
        } else {
            Invoke-Check $Python @('scripts/run_mcp_checks.py', '--build-dir', $BuildDir, '--checks',
                'format', 'lint', 'test', 'fuzz', 'vuln', 'controls')
        }
        $status = 0
    } catch {
        Write-Host $_
    } finally {
        Stop-Transcript | Out-Null
        Set-Content -LiteralPath "$Transcript.exit" -Value $status
    }
    exit $status
}

$transcriptPath = Join-Path ([IO.Path]::GetTempPath()) "hdmapping-mcp-$([Guid]::NewGuid()).log"
$pwsh = (Get-Process -Id $PID).Path
$Python = (Get-Command python -CommandType Application).Source
$CMake = (Get-Command cmake -CommandType Application).Source
$CTest = (Get-Command ctest -CommandType Application).Source
$command = '"{0}" -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "{1}" -BuildDir "{2}" -Config "{3}" -Transcript "{4}" -ExpectedSid "{5}" -Python "{6}" -CMake "{7}" -CTest "{8}"' -f
    $pwsh, $PSCommandPath, $BuildDir, $Config, $transcriptPath, $sid, $Python, $CMake, $CTest
if ($DeployDir) {
    $command += ' -DeployDir "{0}"' -f $DeployDir
}

# Win32_Process.Create uses the same account through the local WMI provider.
# CREATE_BREAKAWAY_FROM_JOB leaves the provider's process quota job.
$startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{
    CreateFlags = [uint32]0x01000000
}
$created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $command
    CurrentDirectory = $root
    ProcessStartupInformation = $startup
}
if ($created.ReturnValue -ne 0) {
    throw "Windows MCP check launcher failed with WMI code $($created.ReturnValue)"
}
$child = $null
$completion = "$transcriptPath.exit"
try {
    $child = Get-Process -Id $created.ProcessId -ErrorAction SilentlyContinue
    if ($child) {
        $null = $child.Handle
        if (-not $child.WaitForExit(600000)) {
            $child.Kill($true)
            throw 'Windows MCP checks exceeded 10 minutes'
        }
    }
    if (-not (Test-Path -LiteralPath $completion)) {
        throw 'Windows MCP checks exited without reporting a result'
    }
    $code = (Get-Content -LiteralPath $completion -Raw).Trim()
    if ($code -ne '0') {
        throw "Windows MCP checks failed with exit code $code"
    }
} finally {
    if ($child) {
        $child.Dispose()
    }
    if (Test-Path -LiteralPath $transcriptPath) {
        Get-Content -LiteralPath $transcriptPath
        Remove-Item -LiteralPath $transcriptPath
    }
    if (Test-Path -LiteralPath $completion) {
        Remove-Item -LiteralPath $completion
    }
}
