# host-tests/compare.ps1
#
# Byte-for-byte comparison of two rendered frames (2048 x 3 bytes RGB888).
# Prints: file sizes, FNV-1a 32-bit checksums, non-black pixel counts,
# number of differing bytes, and the first 10 differing byte indices with
# values. Exits 0 on identical, 1 on mismatch, 2 on missing/uneven files.

param(
    [string]$LegacyBin = "legacy.bin",
    [string]$DirectBin = "direct.bin"
)

$ErrorActionPreference = "Stop"

function Read-Bytes([string]$path) {
    if (-not (Test-Path $path)) {
        Write-Host "ERROR: $path not found"
        exit 2
    }
    return [System.IO.File]::ReadAllBytes($path)
}

function Get-Fnv1a32([byte[]]$data) {
    $h = [uint32]0x811c9dc5
    foreach ($b in $data) {
        $h = $h -bxor $b
        $h = ($h * 0x01000193) -band 0xFFFFFFFF
    }
    return $h
}

function Get-NonBlack([byte[]]$data) {
    $n = 0
    for ($i = 0; $i -lt $data.Length; $i += 3) {
        if ($data[$i] -ne 0 -or $data[$i+1] -ne 0 -or $data[$i+2] -ne 0) { $n++ }
    }
    return $n
}

$a = Read-Bytes $LegacyBin
$b = Read-Bytes $DirectBin

Write-Host ("legacy: {0}  size={1} bytes  fnv1a32=0x{2:X8}  nonBlackPixels={3}" -f (Split-Path $LegacyBin -Leaf), $a.Length, (Get-Fnv1a32 $a), (Get-NonBlack $a))
Write-Host ("direct: {0}  size={1} bytes  fnv1a32=0x{2:X8}  nonBlackPixels={3}" -f (Split-Path $DirectBin -Leaf), $b.Length, (Get-Fnv1a32 $b), (Get-NonBlack $b))

if ($a.Length -ne $b.Length) {
    Write-Host "FAIL: file size mismatch ($($a.Length) vs $($b.Length))"
    exit 1
}

$diffCount = 0
$first10 = @()
for ($i = 0; $i -lt $a.Length; $i++) {
    if ($a[$i] -ne $b[$i]) {
        $diffCount++
        if ($first10.Count -lt 10) {
            $px = [math]::Floor($i / 3)
            $ch = @("R","G","B")[$i % 3]
            $first10 += ("  byte[{0}] (pixel {1} {2}): legacy=0x{3:X2} direct=0x{4:X2}" -f $i, $px, $ch, $a[$i], $b[$i])
        }
    }
}

if ($diffCount -eq 0) {
    Write-Host ""
    Write-Host "PASS: byte-identical output ($($a.Length) bytes, FNV-1a 0x{0:X8})" -f (Get-Fnv1a32 $a)
    Write-Host "      legacy QuadTree ray-cast path == direct triangle rasterizer path"
    exit 0
} else {
    Write-Host ""
    Write-Host "FAIL: $diffCount differing byte(s) out of $($a.Length)"
    Write-Host "First 10 differences:"
    foreach ($line in $first10) { Write-Host $line }
    exit 1
}
