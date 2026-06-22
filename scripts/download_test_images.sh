#!/bin/bash
# Download PNG test image corpora for benchmarking
set -e

TEST_DIR="${1:-test_images}"
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

echo "=== Downloading PNG test image corpora ==="

# PNGSuite - the standard PNG test suite
echo ""
echo "--- PNGSuite ---"
if [ ! -d "PngSuite" ]; then
    wget -q http://www.schaik.com/pngsuite/PngSuite-2017jul19.tgz -O PngSuite.tgz
    tar xzf PngSuite.tgz
    rm PngSuite.tgz
    echo "Downloaded PNGSuite"
else
    echo "PNGSuite already exists"
fi

# ImageCompression.info test images (photographic)
echo ""
echo "--- ImageCompression.info ---"
if [ ! -f "bridge.png" ]; then
    for img in bridge lighthouse faces flowers; do
        wget -q "http://imagecompression.info/test_images/${img}.png" -O "${img}.png" || true
    done
    echo "Downloaded photographic test images"
else
    echo "Photographic images already exist"
fi

# Generate synthetic test images using Python
echo ""
echo "--- Synthetic test images ---"
if command -v python3 &>/dev/null; then
    python3 << 'PYEOF'
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
                raw.extend([x * 8, x * 8, x * 8, 255])
            elif pattern == 'text':
                # Simple text-like pattern
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
        name = f'synth_{pat}_{w}x{h}.png'
        if not os.path.exists(name):
            sz = make_png(w, h, pat, name)
            print(f'  {name}: {sz} bytes')
        else:
            print(f'  {name}: exists')

# Also generate tiny icons
for i in range(1, 9):
    w = h = 2**i
    name = f'synth_icon_{w}x{w}.png'
    if not os.path.exists(name):
        sz = make_png(w, h, 'gradient', name)
        print(f'  {name}: {sz} bytes')
PYEOF
else
    echo "python3 not available - skipping synthetic images"
fi

echo ""
echo "=== Download complete ==="
echo "Images in: $(pwd)"
find . -name '*.png' | wc -l | xargs echo "Total PNG files:"
