#!/usr/bin/env python3
"""Independent, stdlib-only check of the TIFF produced by --self-test."""
import pathlib
import struct
import sys
p = pathlib.Path(sys.argv[1])
b = p.read_bytes()
assert b[:4] == b'II*\x00'
u16 = lambda off: struct.unpack_from('<H', b, off)[0]
u32 = lambda off: struct.unpack_from('<I', b, off)[0]
offset = u32(4)
entries = {}
for i in range(u16(offset)):
    pos = offset + 2 + i * 12
    tag, typ, count = struct.unpack_from('<HHI', b, pos)
    size = {2:1, 3:2, 4:4, 7:1}[typ] * count
    start = pos + 8 if size <= 4 else u32(pos+8)
    entries[tag] = b[start:start+size]
shorts = lambda tag: struct.unpack('<'+'H'*(len(entries[tag])//2), entries[tag])
long = lambda tag: struct.unpack('<I', entries[tag])[0]
assert long(256) == 2 and long(257) == 2
assert shorts(258) == (32,32,32,32)
assert shorts(339) == (3,3,3,3)
assert shorts(338) == (2,)
assert shorts(274) == (1,)
assert len(entries[34675]) > 128
assert entries[34675][36:40] == b'acsp'
expected = [0.18,1,8,1,-0.25,4,0.125,1,2,0.5,16,0.5,0.0625,0.25,0.75,1]
pixel_bytes = b[long(273):long(273)+long(279)]
assert pixel_bytes == struct.pack('<16f', *expected), 'Float bit patterns differ'
print('PASS: independent TIFF tags, ICC signature, row order, alpha and bit-exact float32 data')
