#include <stdio.h>
#define ITEMS 60
#define CAP 4000
int wt[ITEMS];
int val[ITEMS];
int dp[CAP + 1];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
int main()
{
    int i, w, cand;
    rng = 99;
    for (i = 0; i < ITEMS; i++)
    {
        wt[i] = 5 + rnd() % 76;
        val[i] = 10 + rnd() % 191;
    }
    for (i = 0; i < ITEMS; i++)
    {
        for (w = CAP; w >= wt[i]; w--)
        {
            cand = dp[w - wt[i]] + val[i];
            if (cand > dp[w])
                dp[w] = cand;
        }
    }
    printf("%d\n", dp[CAP]);
    return 0;
}
