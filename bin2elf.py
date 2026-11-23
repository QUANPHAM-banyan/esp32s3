import struct

with open("app.bin", "rb") as f:
    data = f.read()

segments = [
    (".flash.rodata", 0x3c0c0020, 0x00000018, 0x28670, 4),
    (".dram0.data",   0x3fc9a600, 0x00028690, 0x07980, 6),
    (".flash.text",   0x42000020, 0x00030018, 0xbe8d0, 5),
    (".dram0.bss_pad",0x3fca1f80, 0x000ee8f0, 0x0012c, 6),
    (".iram0.text",   0x40374000, 0x000eea24, 0x16554, 5),
    (".rtc.data",     0x50000000, 0x00104f80, 0x00024, 6),
]

entry = 0x40375868
ph_off = 52
ph_num = len(segments)
ph_size = 32
cur_file_off = ph_off + ph_num * ph_size

elf_header = struct.pack("<16sHHIIIIIHHHHHH",
    b"\x7fELF\x01\x01\x01\x00" + b"\x00"*8,
    2, 94, 1, entry, ph_off, 0, 0, 52, ph_size, ph_num, 0, 0, 0)

pahrs = b""
body = b""
for name, vaddr, offset, length, flags in segments:
    chunk = data[offset : offset + length]
    pahrs += struct.pack("<IIIIIIII", 1, cur_file_off, vaddr, vaddr, length, length, flags, 4)
    body += chunk
    cur_file_off += length

with open("app.elf", "wb") as f:
    f.write(elf_header + pahrs + body)
print("SUCCESS: app.elf created")
