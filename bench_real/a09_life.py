W = 40
H = 40
rng = 55
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
g1 = [0] * (W * H)
g2 = [0] * (W * H)
for i in range(W * H):
    g1[i] = 1 if rnd() % 3 == 0 else 0
def neighbors(g, x, y):
    n = 0
    for dy in range(-1, 2):
        for dx in range(-1, 2):
            if dx == 0 and dy == 0:
                continue
            nx = x + dx
            ny = y + dy
            if nx >= 0 and nx < W and ny >= 0 and ny < H:
                n += g[ny * W + nx]
    return n
for gen in range(120):
    for y in range(H):
        for x in range(W):
            n = neighbors(g1, x, y)
            if g1[y * W + x]:
                g2[y * W + x] = 1 if (n == 2 or n == 3) else 0
            else:
                g2[y * W + x] = 1 if n == 3 else 0
    for i in range(W * H):
        g1[i] = g2[i]
alive = 0
chk = 0
for i in range(W * H):
    alive += g1[i]
    chk += g1[i] * (i % 11 + 1)
print(alive, chk)
