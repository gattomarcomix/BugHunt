#include <stdio.h>
int count;
int full;
void solve(int cols, int d1, int d2)
{
    int avail, bit;
    if (cols == full)
    {
        count++;
        return;
    }
    avail = full & ~(cols | d1 | d2);
    while (avail != 0)
    {
        bit = avail & (-avail);
        avail = avail - bit;
        solve(cols | bit, ((d1 | bit) << 1) & full, (d2 | bit) >> 1);
    }
}
int main()
{
    int n;
    n = 9;
    full = (1 << n) - 1;
    count = 0;
    solve(0, 0, 0);
    printf("%d\n", count);
    return 0;
}
