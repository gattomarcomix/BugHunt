MAXL = 1600
big = [0] * MAXL
length = 1
big[0] = 1
for n in range(2, 2001):
    carry = 0
    for i in range(length):
        cur = big[i] * n + carry
        big[i] = cur % 10000
        carry = cur // 10000
    while carry > 0:
        big[length] = carry % 10000
        length += 1
        carry = carry // 10000
ds = 0
limbsum = 0
for i in range(length):
    d = big[i]
    while d > 0:
        ds += d % 10
        d = d // 10
    limbsum += big[i]
print(length, ds, limbsum, big[length - 1])
