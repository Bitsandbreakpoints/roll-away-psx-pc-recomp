"""PS-X EXE files: the header and the memory image."""
import struct

HEADER_SIZE = 0x800


class PsxExe:
    def __init__(self, data):
        if data[:8] != b'PS-X EXE':
            raise ValueError('not a PS-X EXE')
        (self.pc, self.gp, self.base, self.size, _, _, self.bss, self.bss_size,
         self.sp, self.sp_size) = struct.unpack_from('<10I', data, 0x10)
        self.image = data[HEADER_SIZE:HEADER_SIZE + self.size]
        if len(self.image) < self.size:
            self.image += bytes(self.size - len(self.image))
        self.end = self.base + self.size

    def contains(self, addr):
        return self.base <= addr < self.end

    def word(self, addr):
        if not self.contains(addr) or addr & 3:
            return None
        return struct.unpack_from('<I', self.image, addr - self.base)[0]

    def words(self):
        """(address, word) of every aligned word of the image."""
        for off in range(0, self.size - 3, 4):
            yield self.base + off, struct.unpack_from('<I', self.image, off)[0]


def load(path):
    with open(path, 'rb') as f:
        return PsxExe(f.read())
