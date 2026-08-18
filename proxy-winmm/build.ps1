<#
.SYNOPSIS
    Builds proxy-winmm into a 64-bit winmm.dll using llvm-mingw (gcc/clang).

.DESCRIPTION
    Statically links the runtime (-static) so the game needs no extra
    runtime DLLs alongside our proxy. Links user32/shell32 (used now) plus
    d3d11/dxgi/ole32 (wired up ahead of the D3D11 hooking work landing in
    later tasks - harmless to link now, unused symbols cost nothing).
#>
$ErrorActionPreference = "Stop"

$out = "$PSScriptRoot\build"
New-Item -ItemType Directory -Force $out | Out-Null

$src = Get-ChildItem "$PSScriptRoot\src\*.c" | ForEach-Object { $_.FullName }
$asm = Get-ChildItem "$PSScriptRoot\src\*.s" | ForEach-Object { $_.FullName }
$def = "$PSScriptRoot\src\winmm.def"

$incArgs = @()
$minhookInc = "$PSScriptRoot\third_party\minhook\include"
if (Test-Path $minhookInc) {
    $incArgs += "-I$minhookInc"
}

Write-Host "Compiling: $(($src + $asm) -join ', ')"
Write-Host "Exports from: $def"

# -shared builds the DLL; -static links libgcc/CRT; output name winmm.dll.
# The .def file (EXPORTS list) makes the raw assembly thunk labels in the
# .s file actually re-exported by name - plain .globl alone is not enough
# for a DLL, unlike __declspec(dllexport) on a C function.
& gcc -O2 -shared -static -o "$out\winmm.dll" @incArgs $src $asm $def `
    -luser32 -lshell32 -ld3d11 -ldxgi -lole32
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "Built: $out\winmm.dll"
