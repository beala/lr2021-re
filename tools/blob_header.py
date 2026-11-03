#!/usr/bin/env python3
"""Write a PRAM blob as a C array of words for MCU firmware that loads it itself.

  python3 tools/blob_header.py firmware/pram/mesh.bin firmware/src/console/mesh_blob.h
"""
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
b = open(src, 'rb').read()
b += bytes(-len(b) % 4)
w = struct.unpack(f'<{len(b) // 4}I', b)
rows = ',\n'.join('  ' + ', '.join(f'0x{x:08X}' for x in w[i:i + 6]) for i in range(0, len(w), 6))
open(dst, 'w').write(f"""// Generated from {src} (the MeshCore repeater blob, docs/pram-repeater.md) for the
// xiao_repeater build. Regenerate after rebuilding mesh.bin:
//   python3 tools/blob_header.py firmware/pram/mesh.bin firmware/src/console/mesh_blob.h
#pragma once
#include <stdint.h>
static const uint32_t MESH_BLOB[{len(w)}] = {{
{rows}
}};
""")
print(f'{dst}: {len(w)} words')
