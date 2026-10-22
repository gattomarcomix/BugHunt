#include <stdio.h>
#define N 110
int A[N * N];
int B[N * N];
int C[N * N];
int main()
{
    int i, j, k, s;
    long long chk, tr;
    for (i = 0; i < N; i++)
    {
        for (j = 0; j < N; j++)
        {
            A[i * N + j] = (i * j + 1) % 10;
            B[i * N + j] = (i + 2 * j) % 9;
        }
    }
    for (i = 0; i < N; i++)
    {
        for (j = 0; j < N; j++)
        {
            s = 0;
            for (k = 0; k < N; k++)
                s += A[i * N + k] * B[k * N + j];
            C[i * N + j] = s;
        }
    }
    chk = 0;
    tr = 0;
    for (i = 0; i < N; i++)
    {
        tr += C[i * N + i];
        for (j = 0; j < N; j++)
            chk += (long long)C[i * N + j] * ((i + j) % 5 + 1);
    }
    printf("%lld %lld\n", tr, chk);
    return 0;
}
