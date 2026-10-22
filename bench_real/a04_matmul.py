N = 110
A = [0] * (N * N)
B = [0] * (N * N)
C = [0] * (N * N)
for i in range(N):
    for j in range(N):
        A[i * N + j] = (i * j + 1) % 10
        B[i * N + j] = (i + 2 * j) % 9
for i in range(N):
    for j in range(N):
        s = 0
        for k in range(N):
            s += A[i * N + k] * B[k * N + j]
        C[i * N + j] = s
chk = 0
tr = 0
for i in range(N):
    tr += C[i * N + i]
    for j in range(N):
        chk += C[i * N + j] * ((i + j) % 5 + 1)
print(tr, chk)
