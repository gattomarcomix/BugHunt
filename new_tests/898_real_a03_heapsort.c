#include <stdio.h>
#define N 5000
int a[N];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536 % 100000;
}
void sift(int *v, int start, int end)
{
    int root, child, t;
    root = start;
    while (root * 2 + 1 <= end)
    {
        child = root * 2 + 1;
        if (child + 1 <= end && v[child] < v[child + 1])
            child++;
        if (v[root] < v[child])
        {
            t = v[root];
            v[root] = v[child];
            v[child] = t;
            root = child;
        }
        else
            return;
    }
}
void heapsort(int *v, int n)
{
    int i, t;
    for (i = (n - 2) / 2; i >= 0; i--)
        sift(v, i, n - 1);
    for (i = n - 1; i > 0; i--)
    {
        t = v[0];
        v[0] = v[i];
        v[i] = t;
        sift(v, 0, i - 1);
    }
}
int main()
{
    int i, ok;
    long long chk;
    rng = 777;
    for (i = 0; i < N; i++)
        a[i] = rnd();
    heapsort(a, N);
    ok = 1;
    chk = 0;
    for (i = 0; i < N; i++)
    {
        if (i > 0 && a[i - 1] > a[i])
            ok = 0;
        chk += (long long)a[i] * (i % 5 + 1);
    }
    printf("%d %lld\n", ok, chk);
    return 0;
}
