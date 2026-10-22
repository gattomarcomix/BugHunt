#include <stdio.h>
#define W 40
#define H 40
int g1[W * H];
int g2[W * H];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
int neighbors(int *g, int x, int y)
{
    int dx, dy, n, nx, ny;
    n = 0;
    for (dy = -1; dy <= 1; dy++)
    {
        for (dx = -1; dx <= 1; dx++)
        {
            if (dx == 0 && dy == 0)
                continue;
            nx = x + dx;
            ny = y + dy;
            if (nx >= 0 && nx < W && ny >= 0 && ny < H)
                n += g[ny * W + nx];
        }
    }
    return n;
}
int main()
{
    int i, gen, x, y, n, alive;
    long long chk;
    rng = 55;
    for (i = 0; i < W * H; i++)
        g1[i] = rnd() % 3 == 0;
    for (gen = 0; gen < 120; gen++)
    {
        for (y = 0; y < H; y++)
        {
            for (x = 0; x < W; x++)
            {
                n = neighbors(g1, x, y);
                if (g1[y * W + x])
                    g2[y * W + x] = (n == 2 || n == 3);
                else
                    g2[y * W + x] = (n == 3);
            }
        }
        for (i = 0; i < W * H; i++)
            g1[i] = g2[i];
    }
    alive = 0;
    chk = 0;
    for (i = 0; i < W * H; i++)
    {
        alive += g1[i];
        chk += g1[i] * (i % 11 + 1);
    }
    printf("%d %lld\n", alive, chk);
    return 0;
}
