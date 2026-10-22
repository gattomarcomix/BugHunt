def gcd(a, b):
    if b == 0:
        return a
    return gcd(b, a % b)
s = 0
cop = 0
for i in range(1, 801):
    for j in range(1, 801):
        g = gcd(i, j)
        s += g
        if g == 1:
            cop += 1
print(s, cop)
