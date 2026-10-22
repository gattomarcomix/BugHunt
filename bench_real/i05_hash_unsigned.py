M = 0xFFFFFFFF
xs = 2463534242
def next_rand():
    global xs
    xs ^= (xs << 13) & M
    xs ^= xs >> 17
    xs ^= (xs << 5) & M
    return xs
def fnv(h, b):
    h ^= b
    h = (h * 16777619) & M
    return h
h = 2166136261
for i in range(500000):
    r = next_rand()
    h = fnv(h, r & 255)
print(xs, h)
