count = 0
n = 12
full = (1 << n) - 1
def solve(cols, d1, d2):
    global count
    if cols == full:
        count += 1
        return
    avail = full & ~(cols | d1 | d2)
    while avail != 0:
        bit = avail & (-avail)
        avail = avail - bit
        solve(cols | bit, ((d1 | bit) << 1) & full, (d2 | bit) >> 1)
solve(0, 0, 0)
print(count)
