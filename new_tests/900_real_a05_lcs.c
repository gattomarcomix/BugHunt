#include <stdio.h>
#define L 150
char s1[L + 1];
char s2[L + 1];
int dp[(L + 1) * (L + 1)];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
int main()
{
    int i, j, w, up, left;
    w = L + 1;
    rng = 4242;
    for (i = 0; i < L; i++)
        s1[i] = 'a' + rnd() % 4;
    for (i = 0; i < L; i++)
        s2[i] = 'a' + rnd() % 4;
    for (i = 1; i <= L; i++)
    {
        for (j = 1; j <= L; j++)
        {
            if (s1[i - 1] == s2[j - 1])
                dp[i * w + j] = dp[(i - 1) * w + (j - 1)] + 1;
            else
            {
                up = dp[(i - 1) * w + j];
                left = dp[i * w + (j - 1)];
                if (up > left)
                    dp[i * w + j] = up;
                else
                    dp[i * w + j] = left;
            }
        }
    }
    printf("%d\n", dp[L * w + L]);
    return 0;
}
