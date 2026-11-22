param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,
    [string]$Binary,
    [string]$OutputDirectory,
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $Version) {
    $definition = Get-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Raw
    $match = [regex]::Match($definition, 'project\(ShutdownWarning VERSION (\d+\.\d+\.\d+)')
    if (-not $match.Success) { throw 'Could not determine the version from CMakeLists.txt.' }
    $Version = $match.Groups[1].Value
}
if (-not $Binary) { $Binary = Join-Path $project 'build/windows/Release/ShutdownWarning.exe' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $project 'dist' }
$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$iconPath = (Resolve-Path -LiteralPath (Join-Path $project 'app/windows/ShutdownWarning.ico')).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$setupPath = Join-Path (Resolve-Path -LiteralPath $OutputDirectory).Path "IdleShutdown-Setup-$Version-x64.exe"

if (-not $Compiler) {
    $onPath = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($onPath) {
        $Compiler = $onPath.Source
    } else {
        $Compiler = Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'
    }
}
if (-not (Test-Path -LiteralPath $Compiler)) {
    throw 'Install NSIS or pass the path to makensis.exe with -Compiler.'
}
$script = Join-Path $PSScriptRoot 'installer.nsi'
& $Compiler /WX "/DAPP_EXE=$binaryPath" "/DAPP_ICON=$iconPath" "/DOUTPUT_FILE=$setupPath" "/DVERSION=$Version" $script
if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }
Get-Item -LiteralPath $setupPath
