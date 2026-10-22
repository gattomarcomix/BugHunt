#include <stdio.h>
#define MAXL 1600
int big[MAXL];
int main()
{
    int n, i, len, carry, cur, ds, d;
    long long limbsum;
    len = 1;
    big[0] = 1;
    for (n = 2; n <= 300; n++)
    {
        carry = 0;
        for (i = 0; i < len; i++)
        {
            cur = big[i] * n + carry;
            big[i] = cur % 10000;
            carry = cur / 10000;
        }
        while (carry > 0)
        {
            big[len] = carry % 10000;
            len = len + 1;
            carry = carry / 10000;
        }
    }
    ds = 0;
    limbsum = 0;
    for (i = 0; i < len; i++)
    {
        d = big[i];
        while (d > 0)
        {
            ds += d % 10;
            d = d / 10;
        }
        limbsum += big[i];
    }
    printf("%d %d %lld %d\n", len, ds, limbsum, big[len - 1]);
    return 0;
}
