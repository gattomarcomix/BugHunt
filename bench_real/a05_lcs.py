L = 850
rng = 4242
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
s1 = [0] * L
s2 = [0] * L
for i in range(L):
    s1[i] = 97 + rnd() % 4
for i in range(L):
    s2[i] = 97 + rnd() % 4
w = L + 1
dp = [0] * (w * w)
for i in range(1, L + 1):
    for j in range(1, L + 1):
        if s1[i - 1] == s2[j - 1]:
            dp[i * w + j] = dp[(i - 1) * w + (j - 1)] + 1
        else:
            up = dp[(i - 1) * w + j]
            left = dp[i * w + (j - 1)]
            if up > left:
                dp[i * w + j] = up
            else:
                dp[i * w + j] = left
print(dp[L * w + L])
