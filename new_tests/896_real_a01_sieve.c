#include <stdio.h>
#define N 120000
char comp[N + 1];
int main()
{
    int i, j, count;
    long long sum;
    for (i = 2; i * i <= N; i++)
    {
        if (!comp[i])
        {
            for (j = i * i; j <= N; j += i)
                comp[j] = 1;
        }
    }
    count = 0;
    sum = 0;
    for (i = 2; i <= N; i++)
    {
        if (!comp[i])
        {
            count++;
            sum += i;
        }
    }
    printf("%d %lld\n", count, sum);
    return 0;
}
