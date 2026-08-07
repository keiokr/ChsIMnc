$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root "nc.c"
$Dist = Join-Path $Root "dist"
$ToolchainRoot = Join-Path $Root "toolchain"
$PortableBin = Join-Path $ToolchainRoot "w64devkit\bin"
$PortableGcc = Join-Path $PortableBin "gcc.exe"
$PortableZig = Join-Path $ToolchainRoot "zig\zig.exe"
$ExeName = if ($env:OUTPUT_NAME) { $env:OUTPUT_NAME } else { "client.exe" }
$ZipName = if ($env:PACKAGE_NAME) { $env:PACKAGE_NAME } else { "package_min.zip" }
$Exe = Join-Path $Dist $ExeName
$Zip = Join-Path $Dist $ZipName

if (!(Test-Path -LiteralPath $Dist)) {
    New-Item -ItemType Directory -Path $Dist | Out-Null
}

Get-ChildItem -LiteralPath $Dist -Force | Remove-Item -Force -Recurse

$gcc = if (Test-Path -LiteralPath $PortableGcc) {
    $PortableGcc
} else {
    (Get-Command gcc -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
}
$zig = if (Test-Path -LiteralPath $PortableZig) { $PortableZig } else { $null }
$cl = Get-Command cl -ErrorAction SilentlyContinue

if ($zig) {
    & $zig cc `
        -target x86_64-windows-gnu `
        -Os `
        -s `
        -DNDEBUG `
        -fno-ident `
        -fno-unwind-tables `
        -fno-asynchronous-unwind-tables `
        -ffunction-sections `
        -fdata-sections `
        "-Wl,--gc-sections" `
        "-Wl,--strip-all" `
        -o $Exe `
        $Src `
        -lws2_32

    if ($LASTEXITCODE -ne 0) {
        throw "zig cc build failed"
    }
}
elseif ($gcc) {
    & $gcc `
        -Os `
        -s `
        -DNDEBUG `
        -fno-ident `
        -fno-unwind-tables `
        -fno-asynchronous-unwind-tables `
        -ffunction-sections `
        -fdata-sections `
        "-Wl,--gc-sections" `
        "-Wl,--strip-all" `
        -o $Exe `
        $Src `
        -lws2_32

    if ($LASTEXITCODE -ne 0) {
        throw "gcc build failed"
    }
}
elseif ($cl) {
    Push-Location $Dist
    try {
        & cl `
            /nologo `
            /O1 `
            /MT `
            /GL `
            /DNDEBUG `
            "/Fe:$Exe" `
            $Src `
            ws2_32.lib `
            /link `
            /OPT:REF `
            /OPT:ICF

        if ($LASTEXITCODE -ne 0) {
            throw "cl build failed"
        }

        Get-ChildItem -LiteralPath $Dist -Include *.obj,*.lib,*.exp,*.pdb,*.ilk -File -Recurse -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    finally {
        Pop-Location
    }
}
else {
    throw "No compiler found. Place a portable Zig or MinGW toolchain under: $ToolchainRoot"
}

if (Test-Path -LiteralPath $Zip) {
    Remove-Item -LiteralPath $Zip -Force
}

Compress-Archive `
    -Path $Exe `
    -DestinationPath $Zip `
    -CompressionLevel Optimal

$exeInfo = Get-Item -LiteralPath $Exe
$zipInfo = Get-Item -LiteralPath $Zip

Write-Host "Built: $($exeInfo.FullName)"
Write-Host "EXE bytes: $($exeInfo.Length)"
Write-Host "Packaged: $($zipInfo.FullName)"
Write-Host "ZIP bytes: $($zipInfo.Length)"
