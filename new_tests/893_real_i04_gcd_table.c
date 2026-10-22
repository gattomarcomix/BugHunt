#include <stdio.h>
int gcd(int a, int b)
{
    if (b == 0)
        return a;
    return gcd(b, a % b);
}
int main()
{
    int i, j, g, cop;
    long long s;
    s = 0;
    cop = 0;
    for (i = 1; i <= 200; i++)
    {
        for (j = 1; j <= 200; j++)
        {
            g = gcd(i, j);
            s += g;
            if (g == 1)
                cop++;
        }
    }
    printf("%lld %d\n", s, cop);
    return 0;
}
