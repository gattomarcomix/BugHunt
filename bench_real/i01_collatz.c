#include <stdio.h>
int steps(long long n)
{
    int c;
    c = 0;
    while (n != 1)
    {
        if (n % 2 == 0)
            n = n / 2;
        else
            n = 3 * n + 1;
        c++;
    }
    return c;
}
int main()
{
    int i, s, best, besti;
    long long total;
    best = 0;
    besti = 0;
    total = 0;
    for (i = 1; i < 60000; i++)
    {
        s = steps(i);
        total += s;
        if (s > best)
        {
            best = s;
            besti = i;
        }
    }
    printf("%d %d %lld\n", besti, best, total);
    return 0;
}
