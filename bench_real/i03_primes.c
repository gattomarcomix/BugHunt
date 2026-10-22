#include <stdio.h>
int isprime(int n)
{
    int d;
    if (n < 2)
        return 0;
    if (n % 2 == 0)
        return n == 2;
    for (d = 3; d * d <= n; d += 2)
    {
        if (n % d == 0)
            return 0;
    }
    return 1;
}
int main()
{
    int n, count;
    long long sum;
    count = 0;
    sum = 0;
    for (n = 0; n < 200000; n++)
    {
        if (isprime(n))
        {
            count++;
            sum += n;
        }
    }
    printf("%d %lld\n", count, sum);
    return 0;
}
