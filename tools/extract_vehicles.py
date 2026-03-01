#!/usr/bin/env python3
"""
Extract all sprites from OpenTTD GRF files (container v1 and v2).
Saves them as individual PNG files into sprites-original/ folder.

Usage:
    python3 extract_vehicles.py [path_to_grf]

Default GRF: /tmp/openttd/baseset/ogfx1_base.grf
"""

import struct
import sys
import os

try:
    from PIL import Image
except ImportError:
    print("Pillow is required: pip3 install Pillow")
    sys.exit(1)

# DOS palette from src/table/palettes.h (256 entries)
DOS_PALETTE = [
    (0,0,0,0),(16,16,16,255),(32,32,32,255),(48,48,48,255),
    (65,64,65,255),(82,80,82,255),(98,101,98,255),(115,117,115,255),
    (131,133,131,255),(148,149,148,255),(168,168,168,255),(184,184,184,255),
    (200,200,200,255),(216,216,216,255),(232,232,232,255),(252,252,252,255),
    (52,60,72,255),(68,76,92,255),(88,96,112,255),(108,116,132,255),
    (132,140,152,255),(156,160,172,255),(176,184,196,255),(204,208,220,255),
    (48,44,4,255),(64,60,12,255),(80,76,20,255),(96,92,28,255),
    (120,120,64,255),(148,148,100,255),(176,176,132,255),(204,204,168,255),
    (72,44,4,255),(88,60,20,255),(104,80,44,255),(124,104,72,255),
    (152,132,92,255),(184,160,120,255),(212,188,148,255),(244,220,176,255),
    (64,0,4,255),(88,4,16,255),(112,16,32,255),(136,32,52,255),
    (160,56,76,255),(188,84,108,255),(204,104,124,255),(220,132,144,255),
    (236,156,164,255),(252,188,192,255),(252,212,0,255),(252,232,60,255),
    (252,248,128,255),(76,40,0,255),(96,60,8,255),(116,88,28,255),
    (136,116,56,255),(156,136,80,255),(176,156,108,255),(196,180,136,255),
    (68,24,0,255),(96,44,4,255),(128,68,8,255),(156,96,16,255),
    (184,120,24,255),(212,156,32,255),(232,184,16,255),(252,212,0,255),
    (252,248,128,255),(252,252,192,255),(32,4,0,255),(64,20,8,255),
    (84,28,16,255),(108,44,28,255),(128,56,40,255),(148,72,56,255),
    (168,92,76,255),(184,108,88,255),(196,128,108,255),(212,148,128,255),
    (8,52,0,255),(16,64,0,255),(32,80,4,255),(48,96,4,255),
    (64,112,12,255),(84,132,20,255),(104,148,28,255),(128,168,44,255),
    (28,52,24,255),(44,68,32,255),(60,88,48,255),(80,104,60,255),
    (104,124,76,255),(128,148,92,255),(152,176,108,255),(180,204,124,255),
    (16,52,24,255),(32,72,44,255),(56,96,72,255),(76,116,88,255),
    (96,136,108,255),(120,164,136,255),(152,192,168,255),(184,220,200,255),
    (32,24,0,255),(56,28,0,255),(72,40,4,255),(88,52,12,255),
    (104,64,24,255),(124,84,44,255),(140,108,64,255),(160,128,88,255),
    (76,40,16,255),(96,52,24,255),(116,68,40,255),(136,84,56,255),
    (164,96,64,255),(184,112,80,255),(204,128,96,255),(212,148,112,255),
    (224,168,128,255),(236,188,148,255),(80,28,4,255),(100,40,20,255),
    (120,56,40,255),(140,76,64,255),(160,100,96,255),(184,136,136,255),
    (36,40,68,255),(48,52,84,255),(64,64,100,255),(80,80,116,255),
    (100,100,136,255),(132,132,164,255),(172,172,192,255),(212,212,224,255),
    (40,20,112,255),(64,44,144,255),(88,64,172,255),(104,76,196,255),
    (120,88,224,255),(140,104,252,255),(160,136,252,255),(188,168,252,255),
    (0,24,108,255),(0,36,132,255),(0,52,160,255),(0,72,184,255),
    (0,96,212,255),(24,120,220,255),(56,144,232,255),(88,168,240,255),
    (128,196,252,255),(188,224,252,255),(16,64,96,255),(24,80,108,255),
    (40,96,120,255),(52,112,132,255),(80,140,160,255),(116,172,192,255),
    (156,204,220,255),(204,240,252,255),(172,52,52,255),(212,52,52,255),
    (252,52,52,255),(252,100,88,255),(252,144,124,255),(252,184,160,255),
    (252,216,200,255),(252,244,236,255),(72,20,112,255),(92,44,140,255),
    (112,68,168,255),(140,100,196,255),(168,136,224,255),(204,180,252,255),
    (204,180,252,255),(232,208,252,255),(60,0,0,255),(92,0,0,255),
    (128,0,0,255),(160,0,0,255),(196,0,0,255),(224,0,0,255),
    (252,0,0,255),(252,80,0,255),(252,108,0,255),(252,136,0,255),
    (252,164,0,255),(252,192,0,255),(252,220,0,255),(252,252,0,255),
    (204,136,8,255),(228,144,4,255),(252,156,0,255),(252,176,48,255),
    (252,196,100,255),(252,216,152,255),(8,24,88,255),(12,36,104,255),
    (20,52,124,255),(28,68,140,255),(40,92,164,255),(56,120,188,255),
    (72,152,216,255),(100,172,224,255),(92,156,52,255),(108,176,64,255),
    (124,200,76,255),(144,224,92,255),(224,244,252,255),(204,240,252,255),
    (180,220,236,255),(132,188,216,255),(88,152,172,255),
    # unused pink (227-238)
    (212,0,212,255),(212,0,212,255),(212,0,212,255),(212,0,212,255),
    (212,0,212,255),(212,0,212,255),(212,0,212,255),(212,0,212,255),
    (212,0,212,255),(212,0,212,255),(212,0,212,255),(212,0,212,255),
    # palette animation (239-254)
    (0,0,0,255),(0,0,0,255),(0,0,0,255),(0,0,0,255),
    (0,0,0,255),(0,0,0,255),(0,0,0,255),(0,0,0,255),
    (0,0,0,255),(0,0,0,255),(0,0,0,255),(0,0,0,255),
    (0,0,0,255),(0,0,0,255),(0,0,0,255),(0,0,0,255),
    # 255: pure white
    (252,252,252,255),
]

GRF_V2_SIG = b'GRF\x82\x0d\x0a\x1a\x0a'


def get_sprite_prefix(sid):
    if sid < 2: return "misc"
    if sid < 142: return "gui"
    if sid < 694: return "gui"
    if sid < 752: return "cursor"
    if sid < 989: return "selection"
    if sid < 1005: return "foundation"
    if sid < 1063: return "rail"
    if sid < 1091: return "station"
    if sid < 1100: return "rail"
    if sid < 1182: return "monorail"
    if sid < 1264: return "maglev"
    if sid < 1332: return "signal"
    if sid < 1408: return "road"
    if sid < 1421: return "excavation"
    if sid < 1700: return "town"
    if sid < 2300: return "industry"
    if sid < 2437: return "misc"
    if sid < 2593: return "bridge"
    if sid < 2733: return "dock"
    if sid < 3090: return "vehicle"
    if sid < 3092: return "vehicle_flag"
    if sid < 3100: return "vehicle"
    if sid < 3900: return "vehicle"
    if sid < 3905: return "rotor"
    if sid < 3924: return "disaster"
    if sid < 4062: return "landscape"
    if sid < 4078: return "water"
    if sid < 4090: return "misc"
    if sid < 4126: return "hedge"
    if sid < 4300: return "farmland"
    if sid < 4404: return "bridge"
    if sid < 4570: return "landscape"
    if sid < 4700: return "town"
    if sid < 4896: return "misc"
    if sid < 4896 + 200: return "openttd"
    return "newgrf"


def decompress_rle(data, expected_size):
    result = bytearray(expected_size)
    src, dst = 0, 0
    while dst < expected_size and src < len(data):
        code = data[src]
        if code > 127: code -= 256
        src += 1
        if code >= 0:
            size = 0x80 if code == 0 else code
            for _ in range(size):
                if src >= len(data) or dst >= expected_size: break
                result[dst] = data[src]; src += 1; dst += 1
        else:
            if src >= len(data): break
            offset = ((code & 7) << 8) | data[src]; src += 1
            size = -(code >> 3)
            for _ in range(size):
                if dst >= expected_size: break
                result[dst] = result[dst - offset] if dst - offset >= 0 else 0
                dst += 1
    return bytes(result)


def decode_chunked(raw, width, height, bpp, use_32bit_hdr):
    pixels = bytearray(width * height * bpp)
    for y in range(height):
        if use_32bit_hdr:
            off = struct.unpack_from('<I', raw, y * 4)[0]
        else:
            off = raw[y * 2] | (raw[y * 2 + 1] << 8)
        pos = off
        while True:
            if use_32bit_hdr:
                if pos + 4 > len(raw): break
                w0 = raw[pos] | (raw[pos+1] << 8)
                w1 = raw[pos+2] | (raw[pos+3] << 8)
                pos += 4
                last_item = (w0 & 0x8000) != 0
                length = w0 & 0x7FFF
                skip = w1
            else:
                if pos + 2 > len(raw): break
                last_item = (raw[pos] & 0x80) != 0
                length = raw[pos] & 0x7F
                skip = raw[pos+1]
                pos += 2
            start = (y * width + skip) * bpp
            for x in range(length):
                for c in range(bpp):
                    if pos < len(raw):
                        idx = start + x * bpp + c
                        if idx < len(pixels):
                            pixels[idx] = raw[pos]
                        pos += 1
            if last_item: break
    return bytes(pixels)


def pixels_to_image(pixel_data, width, height, has_rgb, has_alpha, has_palette, bpp):
    img = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    px = img.load()
    for y in range(height):
        for x in range(width):
            off = (y * width + x) * bpp
            if off + bpp > len(pixel_data): break
            if has_rgb:
                r, g, b = pixel_data[off], pixel_data[off+1], pixel_data[off+2]
                p = 3
                a = pixel_data[off+p] if has_alpha else 255
                if has_alpha: p += 1
                px[x, y] = (r, g, b, a)
            elif has_palette:
                idx = pixel_data[off]
                px[x, y] = DOS_PALETTE[idx] if idx < 256 else (0,0,0,0)
    return img


# ---- Container V1 ----

def extract_v1(f, out_dir):
    f.seek(0)
    sprite_idx, saved = 0, 0
    while True:
        hdr = f.read(2)
        if len(hdr) < 2: break
        num = struct.unpack('<H', hdr)[0]
        if num == 0: break
        grf_type = struct.unpack('B', f.read(1))[0]
        if grf_type == 0xFF:
            f.read(num - 1); sprite_idx += 1; continue
        height = struct.unpack('B', f.read(1))[0]
        width = struct.unpack('<H', f.read(2))[0]
        x_offs = struct.unpack('<h', f.read(2))[0]
        y_offs = struct.unpack('<h', f.read(2))[0]
        if width == 0 or height == 0 or width > 4096 or height > 4096:
            sprite_idx += 1; continue
        compressed = (grf_type & 0x02) != 0
        decomp_size = width * height if compressed else num - 8
        chunked = (grf_type & 0x08) != 0
        if decomp_size <= 0 or decomp_size > 64*1024*1024:
            sprite_idx += 1; continue
        raw_data = f.read(num - 8)
        if len(raw_data) < num - 8: break
        try:
            dec = decompress_rle(raw_data, decomp_size)
            if chunked:
                pd = decode_chunked(dec, width, height, 1, False)
            else:
                pd = dec
            img = pixels_to_image(pd, width, height, False, False, True, 1)
            prefix = get_sprite_prefix(sprite_idx)
            fname = os.path.join(out_dir, f"{prefix}_{sprite_idx}.png")
            img.save(fname); saved += 1
            if saved % 200 == 0:
                print(f"  ... {saved} sprites saved")
        except Exception as e:
            print(f"  skip {sprite_idx}: {e}")
        sprite_idx += 1
    return sprite_idx, saved


# ---- Container V2 ----

def extract_v2(f, out_dir):
    f.seek(0)
    f.read(2)  # 0x0000
    sig = f.read(8)
    if sig != GRF_V2_SIG:
        print("Invalid v2 signature"); return 0, 0

    data_offset = struct.unpack('<I', f.read(4))[0]
    pseudo_start = f.tell()  # position 14
    data_section_start = pseudo_start + data_offset

    # 1) Parse data section: build map of data_id -> file_pos
    f.seek(data_section_start)
    data_sprites = {}  # data_id -> file_pos of the ID dword
    while True:
        pos = f.tell()
        d = f.read(4)
        if len(d) < 4: break
        did = struct.unpack('<I', d)[0]
        if did == 0: break
        if did not in data_sprites:
            data_sprites[did] = pos
        d = f.read(4)
        if len(d) < 4: break
        sz = struct.unpack('<I', d)[0]
        f.seek(f.tell() + sz)

    print(f"  Data section: {len(data_sprites)} unique sprite entries")

    # 2) Parse pseudo-sprite section (after compression byte)
    f.seek(pseudo_start)
    compression = struct.unpack('B', f.read(1))[0]  # must be 0
    if compression != 0:
        print(f"  Unsupported compression: {compression}"); return 0, 0

    sprite_refs = []  # (global_sprite_idx, data_section_id)
    sprite_idx = 0

    while f.tell() < data_section_start:
        d = f.read(4)
        if len(d) < 4: break
        num = struct.unpack('<I', d)[0]
        if num == 0: break
        grf_type = struct.unpack('B', f.read(1))[0]

        if grf_type == 0xFF:
            # Pseudo-sprite: skip remaining `num` bytes (num includes type byte,
            # but we already read 1 byte, so skip num - 1? No...)
            # Actually: num = total data size after the 4-byte length field,
            # type byte is the first byte of that data.
            # So remaining = num - 1
            # BUT looking at OpenTTD code for 0xFF: file.SkipBytes(num)
            # which skips `num` bytes AFTER the type byte.
            # This means num does NOT include the type byte.
            f.read(num)
            sprite_idx += 1
            continue

        if grf_type == 0xFD:
            # Reference to data section sprite
            # num should be 4 (one DWORD ref id, after type byte)
            if num == 4:
                ref_id = struct.unpack('<I', f.read(4))[0]
                sprite_refs.append((sprite_idx, ref_id))
            else:
                f.read(num)
            sprite_idx += 1
            continue

        # Other types: inline sprite (invalid for v2), skip
        f.read(num)
        sprite_idx += 1

    total = sprite_idx
    print(f"  Total sprites: {total}, data refs: {len(sprite_refs)}")

    # 3) Extract all sprites from data section
    saved = 0
    for sprite_idx, ref_id in sprite_refs:
        if ref_id not in data_sprites:
            continue
        try:
            img = read_v2_sprite(f, data_sprites[ref_id])
            if img is not None:
                prefix = get_sprite_prefix(sprite_idx)
                fname = os.path.join(out_dir, f"{prefix}_{sprite_idx}.png")
                img.save(fname); saved += 1
                if saved % 200 == 0:
                    print(f"  ... {saved} sprites saved")
        except Exception as e:
            print(f"  skip {sprite_idx} (ref {ref_id}): {e}")

    return total, saved


def read_v2_sprite(f, file_pos):
    """Read one sprite from the v2 data section. Pick normal zoom (zoom=0)."""
    f.seek(file_pos)
    first_id = struct.unpack('<I', f.read(4))[0]
    best_img = None
    current_id = first_id

    while True:
        d = f.read(4)
        if len(d) < 4: break
        size = struct.unpack('<I', d)[0]
        if size == 0: break
        start = f.tell()

        type_byte = struct.unpack('B', f.read(1))[0]
        if type_byte == 0xFF:
            f.seek(start + size)
            # peek next id
            d = f.read(4)
            if len(d) < 4: break
            nid = struct.unpack('<I', d)[0]
            if nid != current_id: break
            continue

        # SpriteComponent bits: bit0=RGB(enum 0), bit1=Alpha(enum 1), bit2=Palette(enum 2)
        colour_mask = type_byte & 0x07
        sprite_flags = type_byte & ~0x07

        zoom = struct.unpack('B', f.read(1))[0]

        has_rgb     = (colour_mask & 1) != 0
        has_alpha   = (colour_mask & 2) != 0
        has_palette = (colour_mask & 4) != 0
        bpp = (3 if has_rgb else 0) + (1 if has_alpha else 0) + (1 if has_palette else 0)

        if bpp == 0:
            f.seek(start + size)
            d = f.read(4)
            if len(d) < 4: break
            nid = struct.unpack('<I', d)[0]
            if nid != current_id: break
            continue

        height = struct.unpack('<H', f.read(2))[0]
        width  = struct.unpack('<H', f.read(2))[0]
        x_offs = struct.unpack('<h', f.read(2))[0]
        y_offs = struct.unpack('<h', f.read(2))[0]

        if width == 0 or height == 0 or width > 4096 or height > 4096:
            f.seek(start + size)
            d = f.read(4)
            if len(d) < 4: break
            nid = struct.unpack('<I', d)[0]
            if nid != current_id: break
            continue

        chunked = (sprite_flags & 0x08) != 0
        if chunked:
            decomp_size = struct.unpack('<I', f.read(4))[0]
        else:
            decomp_size = width * height * bpp

        remaining = size - (f.tell() - start)
        compressed = f.read(remaining)
        dec = decompress_rle(compressed, decomp_size)

        if chunked:
            use_32bit = (width > 256) or (decomp_size > 0xFFFF)
            pd = decode_chunked(dec, width, height, bpp, use_32bit)
        else:
            pd = dec

        img = pixels_to_image(pd, width, height, has_rgb, has_alpha, has_palette, bpp)

        if zoom == 0 or best_img is None:
            best_img = img

        # peek next id
        d = f.read(4)
        if len(d) < 4: break
        nid = struct.unpack('<I', d)[0]
        if nid != current_id: break

    return best_img


def main():
    default_grf = "/tmp/openttd/baseset/ogfx1_base.grf"
    grf_path = sys.argv[1] if len(sys.argv) > 1 else default_grf

    if not os.path.exists(grf_path):
        print(f"GRF not found: {grf_path}"); sys.exit(1)

    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sprites-original")
    os.makedirs(out_dir, exist_ok=True)

    print(f"Reading: {grf_path}")
    print(f"Output:  {out_dir}")
    print()

    with open(grf_path, 'rb') as f:
        first_word = struct.unpack('<H', f.read(2))[0]
        f.seek(0)
        if first_word == 0:
            f.read(2); sig = f.read(8); f.seek(0)
            if sig == GRF_V2_SIG:
                print("Container v2")
                total, saved = extract_v2(f, out_dir)
            else:
                print("Unknown format"); sys.exit(1)
        else:
            print("Container v1")
            total, saved = extract_v1(f, out_dir)

    print(f"\nDone! Sprites in file: {total}, vehicle saved: {saved}")
    print(f"Output: {out_dir}")


if __name__ == '__main__':
    main()
