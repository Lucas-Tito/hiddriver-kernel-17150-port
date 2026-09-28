"""Helpers for porting hiddriver addresses: memory images, signature scan, PPC disassembly."""
import os
import re
import struct
import capstone

HERE = os.path.dirname(os.path.abspath(__file__))


class Image:
    def __init__(self, path):
        name = os.path.basename(path)
        self.base = int(re.search(r"_([0-9A-Fa-f]{8})\.bin$", name).group(1), 16)
        self.data = open(path, "rb").read()
        self.name = name

    def contains(self, addr):
        return self.base <= addr < self.base + len(self.data)

    def u32(self, addr):
        o = addr - self.base
        return struct.unpack(">I", self.data[o:o + 4])[0]

    def bytes(self, addr, n):
        o = addr - self.base
        return self.data[o:o + n]

    def scan(self, sig):
        """sig: 'AA BB ? CC' (? or ?? = wildcard). Returns matching addresses."""
        parts = sig.split()
        rx = b"".join(b"." if p.startswith("?") else re.escape(bytes([int(p, 16)])) for p in parts)
        return [self.base + m.start() for m in re.finditer(rx, self.data, re.S)]


_md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
_md.detail = False


def disasm(img, addr, count=20):
    out = []
    code = img.bytes(addr, count * 4)
    for i in range(count):
        a = addr + i * 4
        word = code[i * 4:i * 4 + 4]
        ins = list(_md.disasm(word, a))
        txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else ".long 0x" + word.hex()
        out.append(f"{a:08X}: {word.hex()}  {txt}")
    return "\n".join(out)


def find_function_start(img, addr, limit=0x2000):
    """Walk back to the previous 'mflr r12' (7D8802A6), the usual prologue start."""
    a = addr & ~3
    while a > addr - limit:
        if img.u32(a) == 0x7D8802A6:
            return a
        a -= 4
    return None


def load():
    kernel = Image(os.path.join(HERE, "17150", "kernel_80040000.bin"))
    xam = Image(os.path.join(HERE, "17150", "xam_815F0000.bin"))
    return kernel, xam
