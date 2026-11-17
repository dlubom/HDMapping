# Run the Windows Server product checks under the runner account, with a
# process lifetime independent of the Actions step's Job Object.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $BuildDir,
    [Parameter(Mandatory)][string] $Config,
    [Parameter(Mandatory)][string] $DeployDir,
    [string] $Transcript,
    [string] $ExpectedSid
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$DeployDir = (Resolve-Path -LiteralPath $DeployDir).Path
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
        Write-Host "Windows MCP checks on $([Environment]::OSVersion.Version), $env:PROCESSOR_ARCHITECTURE, SID $sid"
        Invoke-Check 'python' @('scripts/run_mcp_checks.py', '--build-dir', $BuildDir, '--checks',
            'format', 'lint', 'test', 'fuzz', 'vuln', 'controls')
        Invoke-Check 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure')
        $relocation = @('scripts/check_mcp_install.py', '--build-dir', $BuildDir, '--config', $Config, '--mcp', 'ON')
        Invoke-Check 'python' $relocation
        Invoke-Check 'python' ($relocation + @('--package-dir', $DeployDir, '--package-executable', 'hdmapping_mcp.exe'))
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
$command = '"{0}" -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "{1}" -BuildDir "{2}" -Config "{3}" -DeployDir "{4}" -Transcript "{5}" -ExpectedSid "{6}"' -f
    $pwsh, $PSCommandPath, $BuildDir, $Config, $DeployDir, $transcriptPath, $sid

$keys = @('Path', 'SystemRoot', 'ComSpec', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA',
    'ProgramFiles', 'ProgramFiles(x86)', 'PROCESSOR_ARCHITECTURE', 'TEMP', 'TMP')
$environment = @('PYTHONUTF8=1')
foreach ($key in $keys) {
    $value = [Environment]::GetEnvironmentVariable($key)
    if ($value) {
        $environment += "$key=$value"
    }
}
# Win32_Process.Create selects the current user via the local WMI provider.
# The provider's startup flags request CREATE_BREAKAWAY_FROM_JOB and Unicode env.
$startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{
    CreateFlags = [uint32]0x01000400
    EnvironmentVariables = [string[]]$environment
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
