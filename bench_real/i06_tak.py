def tak(x, y, z):
    if y >= x:
        return z
    return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y))
s = 0
for i in range(300):
    s += tak(18, 12, 6 + i % 3)
print(s)
