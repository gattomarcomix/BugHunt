N = 40000
rng = 8
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
a = [0] * N
for i in range(1, N):
    a[i] = a[i - 1] + 1 + rnd() % 5
def find(v, n, key):
    lo = 0
    hi = n - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if v[mid] == key:
            return mid
        if v[mid] < key:
            lo = mid + 1
        else:
            hi = mid - 1
    return -1
hist = [0] * 64
found = 0
s = 0
for i in range(150000):
    key = rnd() % (a[N - 1] + 1)
    idx = find(a, N, key)
    if idx >= 0:
        found += 1
        s += idx
        hist[idx % 64] += 1
print(found, s, hist[7])
