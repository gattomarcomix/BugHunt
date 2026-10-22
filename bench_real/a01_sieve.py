N = 1200000
comp = [0] * (N + 1)
i = 2
while i * i <= N:
    if not comp[i]:
        j = i * i
        while j <= N:
            comp[j] = 1
            j += i
    i += 1
count = 0
s = 0
for i in range(2, N + 1):
    if not comp[i]:
        count += 1
        s += i
print(count, s)
