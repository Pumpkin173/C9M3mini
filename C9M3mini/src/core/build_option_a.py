import struct
import sys
import os

from rebuilder import ifd0, raw_ifd

def pack_entry(tag, typ, count, val_offset):
    return struct.pack("<HHLL", tag, typ, count, val_offset)

def build_dng():
    # 1. Prepare payload arrays
    payload_data = bytearray()
    
    # We will write:
    # 0x00: TIFF Header (8 bytes) -> Points to IFD0 at 0x08
    # 0x08: IFD0
    # Next, IFD0 Payloads
    # Next, RAW_IFD (SubIFD)
    # Next, RAW_IFD Payloads
    # Pad to 1024
    
    # We need to compute offsets in two passes.
    # Pass 1: measure sizes
    ifd0_clean = []
    # Strip out thumbnail (tag 273 StripOffsets and 279 StripByteCounts) from IFD0
    # Wait, we need to provide a 1x1 black RGB pixel for IFD0
    dummy_pixel_offset = 0 # Will be resolved
    
    for entry in ifd0:
        if entry['tag'] in (254, 256, 257, 258, 259, 262, 273, 277, 279, 284, 330, 34665, 33421, 33422, 50713, 50714, 50717):
            continue
        ifd0_clean.append(entry)
        
    for entry in raw_ifd:
        if entry['tag'] in (273, 279):
            continue
        ifd0_clean.append(entry)
        
    # Standard RAW strip offsets
    ifd0_clean.append({'tag': 273, 'type': 4, 'count': 1, 'val_offset': 1024, 'byte_size': 4, 'payload': None}) # The RAW data starts exactly at 1024
    ifd0_clean.append({'tag': 279, 'type': 4, 'count': 1, 'val_offset': 3168256, 'byte_size': 4, 'payload': None})
        
    # Inject Exposure Data Tags
    ifd0_clean.append({'tag': 33434, 'type': 5, 'count': 1, 'val_offset': 0, 'byte_size': 8, 'payload': struct.pack("<II", 1, 100)}) # ExposureTime (RATIONAL)
    ifd0_clean.append({'tag': 34855, 'type': 3, 'count': 1, 'val_offset': 100, 'byte_size': 4, 'payload': None}) # PhotographicSensitivity (SHORT)
    ifd0_clean.append({'tag': 33437, 'type': 5, 'count': 1, 'val_offset': 0, 'byte_size': 8, 'payload': struct.pack("<II", 18, 10)}) # FNumber (RATIONAL)

    ifd0_clean.sort(key=lambda x: x['tag'])

    # Pass 2: Layout
    offset = 8 # After TIFF header
    
    # Size of IFD0: 2 bytes count + len*12 + 4 bytes next_ptr
    ifd0_size = 2 + len(ifd0_clean) * 12 + 4
    offset += ifd0_size
    
    # Allocate IFD0 payloads
    for entry in ifd0_clean:
        if entry['payload'] is not None and entry['byte_size'] > 4:
            entry['final_payload_pos'] = offset
            offset += entry['byte_size']
            # Align to 2 bytes
            if offset % 2 != 0: offset += 1

    # Let's BUILD the exact binary!
    out = bytearray(1024)
    out[0:8] = struct.pack("<II", 0x002A4949, 8) # TIFF Header, First IFD at 8
    
    as_shot_neutral_offset = 0
    exposure_time_offset = 0
    iso_offset = 0

    # Write IFD0
    pos = 8
    struct.pack_into("<H", out, pos, len(ifd0_clean)); pos += 2
    for entry in ifd0_clean:
        v = entry['val_offset']
        if entry.get('final_payload_pos'):
            v = entry['final_payload_pos']
            # Write payload
            p = entry['payload']
            out[v:v+len(p)] = p
            if entry['tag'] == 50728: # AsShotNeutral
                as_shot_neutral_offset = v
            if entry['tag'] == 33434: # ExposureTime (RATIONAL payload)
                exposure_time_offset = v
        
        if entry['tag'] == 34855: # ISO, PhotographicSensitivity (SHORT, packed in offset)
            iso_offset = pos + 8
        
        struct.pack_into("<HHLL", out, pos, entry['tag'], entry['type'], entry['count'], v)
        pos += 12
    struct.pack_into("<L", out, pos, 0) # Next IFD = 0
    
    print(f"Generated DNG_PREFIX up to size {offset} bytes. Padding to 1024.")
    print(f"AsShotNeutral Offset: {as_shot_neutral_offset}")
    print(f"ExposureTime Offset: {exposure_time_offset}")
    print(f"ISO Offset: {iso_offset}")

    # Generate C++ Code!
    c_code = "#pragma once\n#include <cstdint>\n\n// Hollywood Standard DNG Header (Zero-Thumbnail, SubIFD Tree)\n"
    c_code += f"const uint32_t DNG_PREFIX_SIZE = 1024;\n"
    c_code += f"// Absolute offset of AsShotNeutral from the START of prefix\n"
    c_code += f"#define DNG_AS_SHOT_NEUTRAL_SUFFIX_OFFSET {as_shot_neutral_offset}\n"
    if exposure_time_offset:
        c_code += f"// Exposure Time rational offset starting at Prefix\n"
        c_code += f"#define DNG_EXPOSURE_TIME_OFFSET {exposure_time_offset}\n"
    if iso_offset:
        c_code += f"// ISO offset starting at Prefix\n"
        c_code += f"#define DNG_ISO_OFFSET {iso_offset}\n"
    c_code += "const uint8_t DNG_PREFIX[1024] = {\n    "
    for i in range(1024):
        c_code += f"0x{out[i]:02X}, "
        if (i+1) % 16 == 0:
            c_code += "\n    "
    c_code += "\n};\n"
    
    with open("DngTemplate.h", "w") as f:
        f.write(c_code)
    print("DngTemplate.h generated successfully!")

if __name__ == '__main__':
    build_dng()
