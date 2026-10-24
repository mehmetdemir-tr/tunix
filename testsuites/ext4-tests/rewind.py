import struct
import sys

def crc32c(crc, data):
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return crc & 0xFFFFFFFF

def main():
    image, offset, sequence = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    with open(image, "r+b") as disk:
        disk.seek(offset)
        block = bytearray(disk.read(1024))
        magic, kind = struct.unpack(">II", block[0:8])
        if magic != 0xC03B3998 or kind != 4:
            sys.exit("not a v2 journal superblock")
        struct.pack_into(">II", block, 24, sequence, 1)
        incompat = struct.unpack(">I", block[40:44])[0]
        if incompat & 0x18:
            struct.pack_into(">I", block, 0xFC, 0)
            struct.pack_into(">I", block, 0xFC, crc32c(0xFFFFFFFF, bytes(block)))
        disk.seek(offset)
        disk.write(block)

main()
