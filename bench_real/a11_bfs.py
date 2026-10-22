W = 200
H = 200
rng = 1234
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
wall = [0] * (W * H)
for i in range(W * H):
    wall[i] = 1 if (rnd() % 100) < 25 else 0
dist = [0] * (W * H)
q = [0] * (W * H)
def bfs(start):
    for i in range(W * H):
        dist[i] = -1
    head = 0
    tail = 0
    q[tail] = start
    tail += 1
    dist[start] = 0
    reach = 0
    maxd = 0
    while head < tail:
        cur = q[head]
        head += 1
        reach += 1
        if dist[cur] > maxd:
            maxd = dist[cur]
        x = cur % W
        y = cur // W
        if x > 0:
            nxt = cur - 1
            if not wall[nxt] and dist[nxt] < 0:
                dist[nxt] = dist[cur] + 1
                q[tail] = nxt
                tail += 1
        if x < W - 1:
            nxt = cur + 1
            if not wall[nxt] and dist[nxt] < 0:
                dist[nxt] = dist[cur] + 1
                q[tail] = nxt
                tail += 1
        if y > 0:
            nxt = cur - W
            if not wall[nxt] and dist[nxt] < 0:
                dist[nxt] = dist[cur] + 1
                q[tail] = nxt
                tail += 1
        if y < H - 1:
            nxt = cur + W
            if not wall[nxt] and dist[nxt] < 0:
                dist[nxt] = dist[cur] + 1
                q[tail] = nxt
                tail += 1
    return reach, maxd
total = 0
mdsum = 0
starts = 0
i = 0
while i < W * H and starts < 16:
    if not wall[i]:
        r, md = bfs(i)
        total += r
        mdsum += md
        starts += 1
    i += 997
print(starts, total, mdsum)
