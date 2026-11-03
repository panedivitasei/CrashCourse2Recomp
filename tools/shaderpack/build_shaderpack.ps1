# Builds out/shaderpack/cc2.pack from your own game files: the game's precache plus the shaders embedded in
# the xex image, translated to DXIL + SPIR-V. Run after building the game.
param(
    [string]$Misses = '',  # the runtime's gpu_shader_cache/<abi>/misses, appended to the deltas CSV first
    [string]$BuildDir = ''  # game build folder whose crashcourse2.exe dumps the image
)
$ErrorActionPreference = 'Continue'
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    # Any 2022 edition will do, as long as it has the C++ workload.
    $vs = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022' -Directory -ErrorAction SilentlyContinue |
          Where-Object { Test-Path "$($_.FullName)\VC\Tools\Llvm\x64\bin\clang.exe" } |
          Select-Object -First 1 -ExpandProperty FullName
    if (-not $vs) { 'Visual Studio 2022 with the C++ workload not found; run this from a Developer PowerShell'; exit 1 }
    $env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
}
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDir) { $BuildDir = "$root\out\build\win-amd64-relwithdebinfo" }
$BuildDir = (Resolve-Path $BuildDir).Path
$game = "$BuildDir\crashcourse2.exe"
$xrDir = "$root\tools\XenosRecomp"
$xrBuild = "$root\out\build\xenosrecomp"
$xr = "$xrBuild\XenosRecomp\XenosRecomp.exe"
$image = "$root\out\cc2_image.bin"
$precache = "$root\assets\data\art\gs\precache_xenon.bsh"
$corpus = "$root\out\cc2_shader_corpus.bin"
$pack = "$root\out\shaderpack\cc2.pack"

if (-not (Test-Path $game)) { "crashcourse2.exe missing in $BuildDir - build the game first"; exit 1 }
if (-not (Test-Path $precache)) { "assets\data\art\gs\precache_xenon.bsh missing - check your game data"; exit 1 }

"=== image dump (the game's dump_image_path writes the decrypted, patched image and exits)"
Remove-Item $image -ErrorAction SilentlyContinue
Start-Process -FilePath $game -ArgumentList "--dump_image_path=$image" -WorkingDirectory $BuildDir -Wait
if (-not (Test-Path $image)) { "image dump failed"; exit 1 }

# One scan input: the patched image (engine shaders in .rdata) followed by the
# game's shader precache.
$out = [System.IO.File]::Create($corpus)
foreach ($f in @($image, $precache)) {
    $bytes = [System.IO.File]::ReadAllBytes($f)
    $out.Write($bytes, 0, $bytes.Length)
}
$out.Close()

"=== XenosRecomp build"
# The pack's ABI hash covers the translator sources, so the tool is rebuilt whenever they change.
if (-not (Test-Path "$xrBuild\build.ninja")) {
    cmake -S $xrDir -B $xrBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { "XenosRecomp configure failed"; exit 1 }
}
cmake --build $xrBuild | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { "XenosRecomp build failed"; exit 1 }

"=== shader pack"
if ($Misses) {
    & $xr --add-misses $corpus $Misses "$root\tools\shaderpack\cc2_shader_deltas.csv"
    if ($LASTEXITCODE -ne 0) { "adding the pack misses failed"; exit 1 }
}
# The CSV lists the runtime shaders; the patches column rebuilds each runtime ucode from its container.
& $xr --cc2-pack $corpus "$root\tools\shaderpack\cc2_shader_deltas.csv" $pack
if ($LASTEXITCODE -ne 0) { "pack generation failed"; exit 1 }

Remove-Item $image, $corpus -ErrorAction SilentlyContinue
"shader pack ready: $pack"
