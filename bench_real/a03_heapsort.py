N = 80000
a = [0] * N
rng = 777
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536 % 100000
def sift(v, start, end):
    root = start
    while root * 2 + 1 <= end:
        child = root * 2 + 1
        if child + 1 <= end and v[child] < v[child + 1]:
            child += 1
        if v[root] < v[child]:
            t = v[root]
            v[root] = v[child]
            v[child] = t
            root = child
        else:
            return
def heapsort(v, n):
    i = (n - 2) // 2
    while i >= 0:
        sift(v, i, n - 1)
        i -= 1
    i = n - 1
    while i > 0:
        t = v[0]
        v[0] = v[i]
        v[i] = t
        sift(v, 0, i - 1)
        i -= 1
for i in range(N):
    a[i] = rnd()
heapsort(a, N)
ok = 1
chk = 0
for i in range(N):
    if i > 0 and a[i - 1] > a[i]:
        ok = 0
    chk += a[i] * (i % 5 + 1)
print(ok, chk)
