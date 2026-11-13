"""Lists the files in a .nds ROM's NitroFS: path -> (offset, size)."""
import struct


def read_fs(data):
    h = data[:0x200]
    fnt_off, fnt_size, fat_off, fat_size = struct.unpack("<4I", h[0x40:0x50])
    fat = [struct.unpack("<II", data[fat_off + i * 8:fat_off + i * 8 + 8]) for i in range(fat_size // 8)]
    files = {}

    def walk(dir_id, prefix):
        entry = fnt_off + (dir_id & 0xFFF) * 8
        sub_off, first_id, _ = struct.unpack("<IHH", data[entry:entry + 8])
        p = fnt_off + sub_off
        fid = first_id
        while True:
            t = data[p]
            p += 1
            if t == 0:
                break
            n = t & 0x7F
            name = data[p:p + n].decode("latin-1")
            p += n
            if t & 0x80:
                sub = struct.unpack("<H", data[p:p + 2])[0]
                p += 2
                walk(sub, prefix + name + "/")
            else:
                s, e = fat[fid]
                files[prefix + name] = (s, e - s)
                fid += 1
    walk(0xF000, "")
    return files
