"""Generate app.ico for light_mp3 — play-button inside a circle, multiple sizes."""
import struct, math

def render_icon(size):
    """Return (pixels_bgra_bottomup, and_mask) for a single icon size."""
    px = bytearray(size * size * 4)
    cx = cy = size / 2.0
    R = size / 2.0 - 0.5

    for row in range(size):
        for col in range(size):
            # BMP is bottom-up
            out_row = size - 1 - row
            idx = (out_row * size + col) * 4

            x = col + 0.5
            y = row + 0.5
            dx = x - cx
            dy = y - cy
            dist = math.sqrt(dx * dx + dy * dy)

            if dist > R + 0.7:
                # Outside — transparent
                px[idx:idx+4] = b'\x00\x00\x00\x00'
                continue

            # Normalised position for gradient
            t = ((col + row) / (2.0 * size))

            # Circle fill: dark-to-bright teal gradient
            br = int(15 + t * 20)
            bg = int(55 + t * 55)
            bb = int(85 + t * 65)

            # Play triangle (equilateral-ish, pointing right)
            nx = col / size        # 0..1
            ny = row / size        # 0..1
            tri_l, tri_r = 0.36, 0.73
            tri_cy = 0.50
            tri_half = 0.27        # half-height at left edge

            in_tri = False
            if tri_l <= nx <= tri_r:
                progress = (nx - tri_l) / (tri_r - tri_l)
                half_h = tri_half * (1.0 - progress)
                if abs(ny - tri_cy) <= half_h:
                    in_tri = True

            # Anti-alias the circle edge
            alpha = 255
            if dist > R - 0.7:
                alpha = max(0, min(255, int(255 * (R + 0.7 - dist) / 1.4)))

            if in_tri:
                # White play symbol
                px[idx:idx+4] = bytes([255, 255, 255, alpha])
            else:
                px[idx:idx+4] = bytes([bb, bg, br, alpha])

    # AND mask: 1-bit per pixel, rows padded to 4-byte boundary
    row_bytes = ((size + 31) // 32) * 4
    mask = bytearray(row_bytes * size)
    for row in range(size):
        out_row = size - 1 - row
        for col in range(size):
            a = px[(out_row * size + col) * 4 + 3]
            if a < 128:
                byte_i = row * row_bytes + col // 8
                mask[byte_i] |= 1 << (7 - col % 8)

    return bytes(px), bytes(mask)


def build_ico(filename, sizes=(16, 32, 48)):
    images = []
    for s in sizes:
        pixels, mask = render_icon(s)
        bih = struct.pack('<IiiHHIIiiII',
                          40, s, s * 2, 1, 32, 0,
                          len(pixels) + len(mask), 0, 0, 0, 0)
        images.append(bih + pixels + mask)

    # ICONDIR + ICONDIRENTRYs
    hdr = struct.pack('<HHH', 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b''
    for i, s in enumerate(sizes):
        w = s if s < 256 else 0
        entries += struct.pack('<BBBBHHII',
                               w, w, 0, 0, 1, 32, len(images[i]), offset)
        offset += len(images[i])

    with open(filename, 'wb') as f:
        f.write(hdr + entries)
        for img in images:
            f.write(img)
    print(f"Wrote {filename}  ({len(sizes)} sizes: {', '.join(str(s) for s in sizes)})")


if __name__ == '__main__':
    build_ico('res/app.ico')
