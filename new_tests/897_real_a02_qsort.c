#include <stdio.h>
#define N 6000
int a[N];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536 % 100000;
}
void qs(int *v, int lo, int hi)
{
    int p, i, j, t;
    if (lo >= hi)
        return;
    p = v[(lo + hi) / 2];
    i = lo;
    j = hi;
    while (i <= j)
    {
        while (v[i] < p)
            i++;
        while (v[j] > p)
            j--;
        if (i <= j)
        {
            t = v[i];
            v[i] = v[j];
            v[j] = t;
            i++;
            j--;
        }
    }
    qs(v, lo, j);
    qs(v, i, hi);
}
int main()
{
    int i, ok;
    long long chk;
    rng = 12345;
    for (i = 0; i < N; i++)
        a[i] = rnd();
    qs(a, 0, N - 1);
    ok = 1;
    chk = 0;
    for (i = 0; i < N; i++)
    {
        if (i > 0 && a[i - 1] > a[i])
            ok = 0;
        chk += (long long)a[i] * (i % 7 + 1);
    }
    printf("%d %lld\n", ok, chk);
    return 0;
}
