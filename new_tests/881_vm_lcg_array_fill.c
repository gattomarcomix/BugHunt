#include <stdio.h>
int a[8];
long long g;
int rnd()
{
    g = (g * 1103515245 + 12345) % 2147483648;
    return g / 65536 % 100;
}
int main()
{
    int i;
    long long s;
    g = 7;
    for (i = 0; i < 8; i++)
        a[i] = rnd();
    s = 0;
    for (i = 0; i < 8; i++)
        s += a[i] * (i + 1);
    printf("%lld\n", s);
    return 0;
}
