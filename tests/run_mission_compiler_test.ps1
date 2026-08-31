$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repo "Sunrise\src"
$luaRoot = Join-Path $repo "Sunrise\vendor\lua-5.4.9\src"
$temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $temporaryRoot "sunrise-mission-compiler-test-$PID"))
if (-not $outputDirectory.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The test output directory must remain below the system temporary directory."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
$luaSources = @(
    "lapi.c", "lauxlib.c", "lcode.c", "lctype.c", "ldebug.c", "ldo.c", "ldump.c",
    "lfunc.c", "lgc.c", "llex.c", "lmem.c", "lobject.c", "lopcodes.c", "lparser.c",
    "lstate.c", "lstring.c", "ltable.c", "ltm.c", "lundump.c", "lvm.c", "lzio.c"
) | ForEach-Object { Join-Path $luaRoot $_ }

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
try {
    $luaArguments = ($luaSources | ForEach-Object { '"{0}"' -f $_ }) -join ' '
    $cppArguments = @(
        (Join-Path $PSScriptRoot "mission_compiler_test.cpp"),
        (Join-Path $sourceRoot "server\gameplay\mission\mission_compiler.cpp")
    ) | ForEach-Object { '"{0}"' -f $_ }
    $cppArguments = $cppArguments -join ' '
    $command = 'cd /d "{0}" && "{1}" >nul && cl.exe /nologo /c /TC /W3 /I"{2}" {3} && cl.exe /nologo /c /std:c++20 /W4 /WX /EHsc /I"{4}" /I"{2}" {5} && link.exe /nologo *.obj /OUT:mission-compiler-test.exe' -f $outputDirectory, $vcvars, $luaRoot, $luaArguments, $sourceRoot, $cppArguments
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "mission compiler test failed to compile." }
    & (Join-Path $outputDirectory "mission-compiler-test.exe") $repo
    if ($LASTEXITCODE -ne 0) { throw "mission compiler test failed." }
} finally {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
