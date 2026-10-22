def steps(n):
    c = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        c += 1
    return c
best = 0
besti = 0
total = 0
for i in range(1, 60000):
    s = steps(i)
    total += s
    if s > best:
        best = s
        besti = i
print(besti, best, total)
