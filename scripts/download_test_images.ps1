# Download PNG test image corpora for benchmarking (Windows/PowerShell)
$ErrorActionPreference = 'Stop'

$TestDir = if ($args.Count -gt 0) { $args[0] } else { 'test_images' }
New-Item -ItemType Directory -Force -Path $TestDir | Out-Null
Push-Location $TestDir

Write-Host "=== Downloading PNG test image corpora ==="

# PNGSuite - the standard PNG test suite
Write-Host ""
Write-Host "--- PNGSuite ---"
if (-not (Test-Path 'PngSuite')) {
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        curl.exe -L -s -o PngSuite.tgz http://www.schaik.com/pngsuite/PngSuite-2017jul19.tgz
    } else {
        Invoke-WebRequest -Uri 'http://www.schaik.com/pngsuite/PngSuite-2017jul19.tgz' -OutFile 'PngSuite.tgz'
    }
    # tar.exe is bundled with Windows 10+ and handles .tgz
    tar.exe -xzf PngSuite.tgz
    Remove-Item 'PngSuite.tgz'
    Write-Host "Downloaded PNGSuite"
} else {
    Write-Host "PNGSuite already exists"
}

# ImageCompression.info test images (photographic)
Write-Host ""
Write-Host "--- ImageCompression.info ---"
if (-not (Test-Path 'bridge.png')) {
    foreach ($img in @('bridge', 'lighthouse', 'faces', 'flowers')) {
        $url = "http://imagecompression.info/test_images/${img}.png"
        try {
            if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
                curl.exe -L -s -o "$img.png" $url
            } else {
                Invoke-WebRequest -Uri $url -OutFile "$img.png"
            }
        } catch {
            Write-Warning "Failed to download $img.png"
        }
    }
    Write-Host "Downloaded photographic test images"
} else {
    Write-Host "Photographic images already exist"
}

# Generate synthetic test images using Python
Write-Host ""
Write-Host "--- Synthetic test images ---"
if (Get-Command python -ErrorAction SilentlyContinue) {
    $py = @'
import struct, zlib, os

def make_png(w, h, pattern, name):
    def chunk(t, d):
        c = t + d
        crc = zlib.crc32(c, 0) & 0xffffffff
        return struct.pack('>I', len(d)) + c + struct.pack('>I', crc)

    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter none
        for x in range(w):
            if pattern == 'gradient':
                raw.extend([x * 255 // w, y * 255 // h, (x + y) * 128 // (w + h), 255])
            elif pattern == 'blocks':
                bx, by = x // (w//4), y // (h//4)
                raw.extend([bx * 85, by * 85, 128, 255])
            elif pattern == 'noise':
                raw.extend([(x*17+y*13)%256, (x*31+y*7)%256, (x*11+y*23)%256, 255])
            elif pattern == 'solid':
                raw.extend([128, 128, 128, 255])
            elif pattern == 'checker':
                c = 255 if (x + y) % 2 == 0 else 0
                raw.extend([c, c, c, 255])
            elif pattern == 'horizontal':
                c = (x * 8) % 256
                raw.extend([c, c, c, 255])
            elif pattern == 'text':
                raw.extend([0, 0, 0, 255] if x % 8 < 2 else [255, 255, 255, 255])
    zdata = zlib.compress(bytes(raw))
    png = sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zdata) + chunk(b'IEND', b'')
    with open(name, 'wb') as f:
        f.write(png)
    return len(png)

sizes = [(16,16), (32,32), (64,64), (128,128), (256,256)]
patterns = ['gradient', 'blocks', 'noise', 'solid', 'checker', 'horizontal', 'text']

for w, h in sizes:
    for pat in patterns:
        name = 'synth_%s_%dx%d.png' % (pat, w, h)
        if not os.path.exists(name):
            sz = make_png(w, h, pat, name)
            print('  %s: %d bytes' % (name, sz))
        else:
            print('  %s: exists' % name)

for i in range(1, 9):
    w = h = 2**i
    name = 'synth_icon_%dx%d.png' % (w, w)
    if not os.path.exists(name):
        sz = make_png(w, h, 'gradient', name)
        print('  %s: %d bytes' % (name, sz))
'@
    $py | python -
} else {
    Write-Host "python not available - skipping synthetic images"
}

Write-Host ""
Write-Host "=== Download complete ==="
Write-Host "Images in: $(Get-Location)"
$count = (Get-ChildItem -Recurse -Filter '*.png').Count
Write-Host "Total PNG files: $count"

Pop-Location