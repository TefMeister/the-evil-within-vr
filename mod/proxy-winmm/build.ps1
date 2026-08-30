<#
.SYNOPSIS
    Builds proxy-winmm into a 64-bit winmm.dll using llvm-mingw (gcc/clang).

.DESCRIPTION
    Statically links the runtime (-static) so the game needs no extra
    runtime DLLs alongside our proxy. Links user32/shell32 (used now) plus
    d3d11/dxgi/ole32 (used by the D3D11 Present hook - dummy device/swapchain
    creation and COM Release calls), dxguid (IID_ID3D11Device, used to
    capture the game's real device from its real swap-chain), and MinHook
    (vendored under third_party/minhook/, function-hooking trampolines for
    the Present hook).
#>
$ErrorActionPreference = "Stop"

$out = "$PSScriptRoot\build"
New-Item -ItemType Directory -Force $out | Out-Null

$src = Get-ChildItem "$PSScriptRoot\src\*.c" | ForEach-Object { $_.FullName }
$asm = Get-ChildItem "$PSScriptRoot\src\*.s" | ForEach-Object { $_.FullName }
$def = "$PSScriptRoot\src\winmm.def"

$minhookSrc = "$PSScriptRoot\third_party\minhook\src"
$minhookInc = "$PSScriptRoot\third_party\minhook\include"

$incArgs = @()
if (Test-Path $minhookInc) {
    $incArgs += "-I$minhookInc"
}

$minhookFiles = @()
if (Test-Path $minhookSrc) {
    $minhookFiles += (Get-ChildItem "$minhookSrc\*.c" | ForEach-Object { $_.FullName })
    $minhookFiles += (Get-ChildItem "$minhookSrc\hde\*.c" | ForEach-Object { $_.FullName })
}

$allSrc = $src + $minhookFiles

Write-Host "Compiling: $(($allSrc + $asm) -join ', ')"
Write-Host "Exports from: $def"

# -shared builds the DLL; -static links libgcc/CRT; output name winmm.dll.
# The .def file (EXPORTS list) makes the raw assembly thunk labels in the
# .s file actually re-exported by name - plain .globl alone is not enough
# for a DLL, unlike __declspec(dllexport) on a C function.
& gcc -O2 -shared -static -o "$out\winmm.dll" @incArgs $allSrc $asm $def `
    -luser32 -lshell32 -ld3d11 -ldxgi -lole32 -ldxguid
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "Built: $out\winmm.dll"
