$ErrorActionPreference = 'Stop'

$repository = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repository 'build\tests\viewer-camera-path-store'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$developerShell = 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat'
$source = Join-Path $PSScriptRoot 'viewer_camera_path_store_tests.cpp'
$store = Join-Path $repository 'Sunrise\src\client\viewer\viewer_camera_path_store.cpp'
$path = Join-Path $repository 'Sunrise\src\core\filesystem\path.cpp'
$temporary = Join-Path $repository 'Sunrise\src\core\filesystem\temporary_sibling.cpp'
$include = Join-Path $repository 'Sunrise\src'
$executable = Join-Path $output 'viewer_camera_path_store_tests.exe'

$command = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{1}" "{2}" "{3}" "{4}" "{5}" /Fo:"{7}\\" /Fe:"{6}"' -f $developerShell, $include, $source, $store, $path, $temporary, $executable, $output
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $executable
exit $LASTEXITCODE
