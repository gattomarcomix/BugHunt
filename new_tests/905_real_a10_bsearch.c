#include <stdio.h>
#define N 8000
int a[N];
int hist[64];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
int find(int *v, int n, int key)
{
    int lo, hi, mid;
    lo = 0;
    hi = n - 1;
    while (lo <= hi)
    {
        mid = (lo + hi) / 2;
        if (v[mid] == key)
            return mid;
        if (v[mid] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}
int main()
{
    int i, key, idx, found;
    long long sum;
    rng = 8;
    a[0] = 0;
    for (i = 1; i < N; i++)
        a[i] = a[i - 1] + 1 + rnd() % 5;
    found = 0;
    sum = 0;
    for (i = 0; i < 15000; i++)
    {
        key = rnd() % (a[N - 1] + 1);
        idx = find(a, N, key);
        if (idx >= 0)
        {
            found++;
            sum += idx;
            hist[idx % 64]++;
        }
    }
    printf("%d %lld %d\n", found, sum, hist[7]);
    return 0;
}
