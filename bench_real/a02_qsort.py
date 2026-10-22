import sys
sys.setrecursionlimit(100000)
N = 100000
a = [0] * N
rng = 12345
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536 % 100000
def qs(v, lo, hi):
    if lo >= hi:
        return
    p = v[(lo + hi) // 2]
    i = lo
    j = hi
    while i <= j:
        while v[i] < p:
            i += 1
        while v[j] > p:
            j -= 1
        if i <= j:
            t = v[i]
            v[i] = v[j]
            v[j] = t
            i += 1
            j -= 1
    qs(v, lo, j)
    qs(v, i, hi)
for i in range(N):
    a[i] = rnd()
qs(a, 0, N - 1)
ok = 1
chk = 0
for i in range(N):
    if i > 0 and a[i - 1] > a[i]:
        ok = 0
    chk += a[i] * (i % 7 + 1)
print(ok, chk)
