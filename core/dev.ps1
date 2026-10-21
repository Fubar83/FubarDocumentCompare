# Utvecklingsmiljö för kärnan: MSVC (vcvars64), vcpkg, CMake och Ninja i den aktuella PowerShell-sessionen.
#   . .\core\dev.ps1            # punkt först: miljön stannar i sessionen
#   cmake --preset windows -S core ; cmake --build core/build/windows ; ctest --test-dir core/build/windows
param([string]$Vcpkg = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "$env:USERPROFILE\vcpkg" }))

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Hittar ingen Visual Studio med C++-verktyg (installera 'Desktop development with C++' / VCTools)" }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:VCPKG_ROOT = $Vcpkg
foreach ($p in @("$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links",
                 (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'))) {
    if ((Test-Path $p) -and ($env:Path -notlike "*$p*")) { $env:Path = "$p;$env:Path" }
}
Write-Host "MSVC $env:VCToolsVersion · vcpkg $env:VCPKG_ROOT · $(cmake --version | Select-Object -First 1)"
