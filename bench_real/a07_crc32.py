N = 500000
M = 0xFFFFFFFF
rng = 31337
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
table = [0] * 256
buf = [0] * N
def make_table():
    for n in range(256):
        c = n
        for k in range(8):
            if c & 1:
                c = 0xEDB88320 ^ (c >> 1)
            else:
                c = c >> 1
        table[n] = c
def crc32(p, n):
    c = 0xFFFFFFFF
    for i in range(n):
        c = table[(c ^ p[i]) & 255] ^ (c >> 8)
    return c ^ 0xFFFFFFFF
for i in range(N):
    buf[i] = rnd() & 255
make_table()
print(crc32(buf, N), crc32(buf, N // 2))
