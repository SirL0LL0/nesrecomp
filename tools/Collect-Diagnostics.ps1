param([Parameter(Mandatory)][string]$Target)
$ErrorActionPreference = 'Stop'
if ([IO.Path]::GetFileName($Target) -cne $Target -or -not $Target.EndsWith('.exe')) {
    throw 'Target must name a game executable beside this script.'
}
$exe = Join-Path $PSScriptRoot $Target
$folder = Join-Path $PSScriptRoot 'diagnostics'
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss-fff') + '-' + $PID
$utf8 = New-Object Text.UTF8Encoding($false)
try {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $report = Join-Path $folder ('loader-' + $stamp + '.log')
    [IO.File]::WriteAllText($report, '', $utf8)
}
catch {
    $folder = Join-Path $env:LOCALAPPDATA 'NESRecomp\diagnostics'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $report = Join-Path $folder ('loader-' + $stamp + '.log')
    [IO.File]::WriteAllText($report, '', $utf8)
}
function Note([string]$Text) { [IO.File]::AppendAllText($report, $Text + [Environment]::NewLine, $utf8) }
Note "Target=$Target UTC=$([DateTime]::UtcNow.ToString('o'))"
Note "OS=$([Environment]::OSVersion.VersionString) architecture=$env:PROCESSOR_ARCHITECTURE CPU=$env:PROCESSOR_IDENTIFIER"
try {
    Get-CimInstance Win32_VideoController | ForEach-Object {
        Note "GPU=$($_.Name) driver=$($_.DriverVersion)"
    }
} catch { Note "GPU query failed: $($_.Exception.Message)" }
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Note 'Executable missing. Extract the complete ZIP before running this helper.'
    Write-Output "Executable missing. Send the diagnostics folder: $folder"
    exit 2
}
Note "Executable SHA256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)"
$outFile = [IO.File]::Create((Join-Path $folder ('loader-' + $stamp + '.stdout.txt')))
$errFile = [IO.File]::Create((Join-Path $folder ('loader-' + $stamp + '.stderr.txt')))
$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = $exe
$info.WorkingDirectory = $PSScriptRoot
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$process = New-Object Diagnostics.Process
$process.StartInfo = $info
$code = 2
try {
    Write-Output 'Starting the game. Reproduce the problem, then close the game if it remains open.'
    [void]$process.Start()
    Note "Process started pid=$($process.Id)"
    $outTask = $process.StandardOutput.BaseStream.CopyToAsync($outFile)
    $errTask = $process.StandardError.BaseStream.CopyToAsync($errFile)
    $process.WaitForExit()
    [void]$outTask.GetAwaiter().GetResult()
    [void]$errTask.GetAwaiter().GetResult()
    $code = $process.ExitCode
    $unsignedCode = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$code), 0)
    Note ('Process exit decimal={0} hex=0x{1:X8}' -f $code, $unsignedCode)
    if ($unsignedCode -eq 3221225781) { Note 'Windows loader: a required DLL was not found (0xC0000135).' }
    if ($unsignedCode -eq 3221225595) { Note 'Windows loader: invalid executable or DLL architecture (0xC000007B).' }
} catch { Note "Launch failed: $($_.Exception.Message)" }
finally { $outFile.Dispose(); $errFile.Dispose(); $process.Dispose() }
Write-Output "Send the diagnostics folder: $folder"
exit $code
