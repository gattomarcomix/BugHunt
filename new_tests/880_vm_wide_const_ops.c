#include <stdio.h>
int f1(long long x)
{
    return x % 2147483648;
}
long long f2(long long x)
{
    return x / 2147483648;
}
long long f3(long long x)
{
    return (x + 4294967296) % 1000;
}
int main()
{
    int i;
    long long s;
    s = 0;
    for (i = 1; i <= 3; i++)
    {
        s += f1(100000000000LL + i);
        s += f2(100000000000LL * i);
        s += f3(i);
    }
    printf("%lld\n", s);
    return 0;
}
