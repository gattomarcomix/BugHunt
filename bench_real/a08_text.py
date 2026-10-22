N = 300000
rng = 2024
def rnd():
    global rng
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng // 65536
txt = [0] * (N + 1)
for i in range(N):
    r = rnd()
    if r % 6 == 0:
        txt[i] = 32
    else:
        txt[i] = 97 + r % 26
txt[N] = 0
def count_words(s):
    words = 0
    length = 0
    longest = 0
    p = 0
    while s[p]:
        if s[p] == 32:
            if length > 0:
                words += 1
                if length > longest:
                    longest = length
            length = 0
        else:
            length += 1
        p += 1
    if length > 0:
        words += 1
        if length > longest:
            longest = length
    return words, longest
def reverse_range(s, a, b):
    while a < b:
        t = s[a]
        s[a] = s[b]
        s[b] = t
        a += 1
        b -= 1
def reverse_words(s):
    p = 0
    while s[p]:
        while s[p] == 32:
            p += 1
        start = p
        while s[p] and s[p] != 32:
            p += 1
        if p > start:
            reverse_range(s, start, p - 1)
words, longest = count_words(txt)
freq = [0] * 26
for i in range(N):
    if txt[i] != 32:
        freq[txt[i] - 97] += 1
reverse_words(txt)
chk = 0
for i in range(N):
    chk += txt[i] * (i % 13 + 1)
k = 0
for i in range(1, 26):
    if freq[i] > freq[k]:
        k = i
print(words, longest, k, freq[k], chk)
