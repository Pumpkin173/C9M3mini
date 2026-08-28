import struct
import sys
import os

from rebuilder import ifd0, raw_ifd

def build_photo_dng():
    ifd0_clean = []
    
    # Keep tags but remove RAW image data tags from IFD0 (which goes to SubIFD)
    for entry in ifd0:
        if entry['tag'] in (273, 279, 330, 258, 259, 262, 277, 284, 34665):
            continue
        ifd0_clean.append(entry)
        
    # Inject JPEG Interchange tags and SubIFDs
    ifd0_clean.append({'tag': 0x0201, 'type': 4, 'count': 1, 'val_offset': 3169280, 'byte_size': 4, 'payload': None})
    ifd0_clean.append({'tag': 0x0202, 'type': 4, 'count': 1, 'val_offset': 0xDEADBEEF, 'byte_size': 4, 'payload': None})
    ifd0_clean.append({'tag': 330, 'type': 13, 'count': 1, 'val_offset': 0, 'byte_size': 4, 'payload': None}) # SubIFDs
    
    # Inject Exposure Data Tags
    ifd0_clean.append({'tag': 33434, 'type': 5, 'count': 1, 'val_offset': 0, 'byte_size': 8, 'payload': struct.pack("<II", 1, 100)}) # ExposureTime (RATIONAL)
    ifd0_clean.append({'tag': 34855, 'type': 3, 'count': 1, 'val_offset': 100, 'byte_size': 4, 'payload': None}) # PhotographicSensitivity (SHORT)
    ifd0_clean.append({'tag': 33437, 'type': 5, 'count': 1, 'val_offset': 0, 'byte_size': 8, 'payload': struct.pack("<II", 18, 10)}) # FNumber (RATIONAL)

    # Update ImageWidth / Length for the thumbnail
    for entry in ifd0_clean:
        if entry['tag'] == 256: # Width
            entry['val_offset'] = 1
        elif entry['tag'] == 257: # Length
            entry['val_offset'] = 1
    
    ifd0_clean.sort(key=lambda x: x['tag'])
    
    raw_ifd_clean = []
    for entry in raw_ifd:
        if entry['tag'] in (273, 279):
            continue
        raw_ifd_clean.append(entry)
        
    # Standard RAW strip offsets
    raw_ifd_clean.append({'tag': 273, 'type': 4, 'count': 1, 'val_offset': 1024, 'byte_size': 4, 'payload': None})
    raw_ifd_clean.append({'tag': 279, 'type': 4, 'count': 1, 'val_offset': 3168256, 'byte_size': 4, 'payload': None})
    
    raw_ifd_clean.sort(key=lambda x: x['tag'])

    offset = 8
    ifd0_size = 2 + len(ifd0_clean) * 12 + 4
    offset += ifd0_size
    
    for entry in ifd0_clean:
        if entry['payload'] is not None and entry['byte_size'] > 4:
            entry['final_payload_pos'] = offset
            offset += entry['byte_size']
            if offset % 2 != 0: offset += 1

    subifd_pos = offset
    raw_ifd_size = 2 + len(raw_ifd_clean) * 12 + 4
    offset += raw_ifd_size

    for entry in raw_ifd_clean:
        if entry['payload'] is not None and entry['byte_size'] > 4:
            entry['final_payload_pos'] = offset
            offset += entry['byte_size']
            if offset % 2 != 0: offset += 1
            
    for entry in ifd0_clean:
        if entry['tag'] == 330:
            entry['val_offset'] = subifd_pos

    out = bytearray(1024)
    out[0:8] = struct.pack("<II", 0x002A4949, 8)
    
    as_shot_neutral_offset = 0
    exposure_time_offset = 0
    iso_offset = 0

    pos = 8
    struct.pack_into("<H", out, pos, len(ifd0_clean)); pos += 2
    for entry in ifd0_clean:
        v = entry['val_offset']
        if entry.get('final_payload_pos'):
            v = entry['final_payload_pos']
            p = entry['payload']
            out[v:v+len(p)] = p
            if entry['tag'] == 50728:
                as_shot_neutral_offset = v
            if entry['tag'] == 33434:
                exposure_time_offset = v
        
        if entry['tag'] == 34855:
            iso_offset = pos + 8
        
        struct.pack_into("<HHLL", out, pos, entry['tag'], entry['type'], entry['count'], v)
        pos += 12
    struct.pack_into("<L", out, pos, 0)
    
    pos = subifd_pos
    struct.pack_into("<H", out, pos, len(raw_ifd_clean)); pos += 2
    for entry in raw_ifd_clean:
        v = entry['val_offset']
        if entry.get('final_payload_pos'):
            v = entry['final_payload_pos']
            p = entry['payload']
            out[v:v+len(p)] = p
            
        struct.pack_into("<HHLL", out, pos, entry['tag'], entry['type'], entry['count'], v)
        pos += 12
    struct.pack_into("<L", out, pos, 0)
    
    c_code = "#pragma once\n#include <cstdint>\n\n// Photo DNG Header with Embedded JPEG\n"
    c_code += f"const uint32_t PHOTO_DNG_PREFIX_SIZE = 1024;\n"
    c_code += f"#define PHOTO_DNG_AS_SHOT_NEUTRAL_SUFFIX_OFFSET {as_shot_neutral_offset}\n"
    c_code += f"#define PHOTO_DNG_EXPOSURE_TIME_OFFSET {exposure_time_offset}\n"
    c_code += f"#define PHOTO_DNG_ISO_OFFSET {iso_offset}\n"
    c_code += "const uint8_t PHOTO_DNG_PREFIX[1024] = {\n    "
    for i in range(1024):
        c_code += f"0x{out[i]:02X}, "
        if (i+1) % 16 == 0:
            c_code += "\n    "
    c_code += "\n};\n"
    
    with open("PhotoDngTemplate.h", "w") as f:
        f.write(c_code)
    print("PhotoDngTemplate.h generated successfully!")

if __name__ == '__main__':
    build_photo_dng()
