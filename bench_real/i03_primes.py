def isprime(n):
    if n < 2:
        return 0
    if n % 2 == 0:
        return 1 if n == 2 else 0
    d = 3
    while d * d <= n:
        if n % d == 0:
            return 0
        d += 2
    return 1
count = 0
s = 0
for n in range(0, 200000):
    if isprime(n):
        count += 1
        s += n
print(count, s)
