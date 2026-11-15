import hashlib
import struct

p = r"romfs and exefs/exefs/main"
d = open(p, "rb").read()
print("file size:", len(d))
print("magic:", d[:4])
print("version:", struct.unpack_from("<I", d, 4)[0])
flags = struct.unpack_from("<I", d, 0x0C)[0]
print("flags:", hex(flags))


def lz4_decompress(src, expected):
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        ll = token >> 4
        if ll == 15:
            while True:
                b = src[i]
                i += 1
                ll += b
                if b != 255:
                    break
        out += src[i:i + ll]
        i += ll
        if i >= len(src):
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - offset
        if start < 0:
            raise ValueError("bad match offset")
        for k in range(ml):
            out.append(out[start + k])
    if len(out) != expected:
        raise ValueError(f"decompressed {len(out)} != expected {expected}")
    return bytes(out)


sections = [
    ("text", 0x10, 0x14, 0x18, 0x60, 0xA0, 1),
    ("rodata", 0x20, 0x24, 0x28, 0x64, 0xC0, 2),
    ("data", 0x30, 0x34, 0x38, 0x68, 0xE0, 4),
]
text = None
for name, fo_key, mo_key, ms_key, fs_key, hf_key, bit in sections:
    file_off = struct.unpack_from("<I", d, fo_key)[0]
    mem_off = struct.unpack_from("<I", d, mo_key)[0]
    mem_size = struct.unpack_from("<I", d, ms_key)[0]
    file_size = struct.unpack_from("<I", d, fs_key)[0]
    compressed = bool(flags & bit)
    raw = d[file_off:file_off + file_size]
    body = lz4_decompress(raw, mem_size) if compressed else raw
    actual = hashlib.sha256(body).hexdigest()
    stored = d[hf_key:hf_key + 32].hex()
    print(f"{name}: file_off=0x{file_off:x} mem_off=0x{mem_off:x} "
          f"mem_size=0x{mem_size:x} file_size=0x{file_size:x} compressed={compressed} "
          f"hash_match={actual == stored}")
    if name == "text":
        text = body

name_off = struct.unpack_from("<I", d, 0x1C)[0]
name_len = struct.unpack_from("<I", d, 0x2C)[0]
print("module name:", d[name_off:name_off + name_len])
print("bss size:", struct.unpack_from("<I", d, 0x3C)[0])

svc = 0
tpidrro = 0
instructions = len(text) // 4
for off in range(0, len(text) - 3, 4):
    insn = struct.unpack_from("<I", text, off)[0]
    if (insn & 0xFFE0001F) == 0xD4000001:
        svc += 1
    if (insn & 0xFFF00000) == 0xD5300000:
        op0 = 2 + ((insn >> 19) & 1)
        op1 = (insn >> 16) & 7
        crn = (insn >> 12) & 0xF
        crm = (insn >> 8) & 0xF
        op2 = (insn >> 5) & 7
        if (op0, op1, crn, crm, op2) == (3, 3, 13, 0, 3):
            tpidrro += 1

print("instructions:", instructions)
print("svc sites:", svc)
print("tpidrro reads:", tpidrro)
print("text sha256:", hashlib.sha256(text).hexdigest())
