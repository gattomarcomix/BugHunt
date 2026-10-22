ITEMS = 300
CAP = 4000
rng = 99
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
wt = [0] * ITEMS
val = [0] * ITEMS
for i in range(ITEMS):
    wt[i] = 5 + rnd() % 76
    val[i] = 10 + rnd() % 191
dp = [0] * (CAP + 1)
for i in range(ITEMS):
    w = CAP
    while w >= wt[i]:
        cand = dp[w - wt[i]] + val[i]
        if cand > dp[w]:
            dp[w] = cand
        w -= 1
print(dp[CAP])
