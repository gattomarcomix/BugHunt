#include <stdio.h>
long long fa(long long x)
{
    return x + 4294967296;
}
long long fb(long long x)
{
    return x - 3000000000;
}
long long fc(long long x)
{
    return x + 2147483648;
}
long long fd(long long x)
{
    return 4294967296 + x;
}
int main()
{
    int i;
    long long s;
    s = 0;
    for (i = 1; i <= 3; i++)
    {
        s += fa(i);
        s += fb(i);
        s += fc(i);
        s += fd(i);
    }
    printf("%lld\n", s);
    return 0;
}
