#include <stdio.h>
int tak(int x, int y, int z)
{
    if (y >= x)
        return z;
    return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
}
int main()
{
    int i;
    long long s;
    s = 0;
    for (i = 0; i < 300; i++)
        s += tak(18, 12, 6 + i % 3);
    printf("%lld\n", s);
    return 0;
}
