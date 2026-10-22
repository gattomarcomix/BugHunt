#include <stdio.h>
#define W 50
#define H 50
int wall[W * H];
int dist[W * H];
int q[W * H];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
int bfs(int start, int *maxd)
{
    int head, tail, cur, x, y, nxt, reach, i;
    for (i = 0; i < W * H; i++)
        dist[i] = -1;
    head = 0;
    tail = 0;
    q[tail] = start;
    tail++;
    dist[start] = 0;
    reach = 0;
    *maxd = 0;
    while (head < tail)
    {
        cur = q[head];
        head++;
        reach++;
        if (dist[cur] > *maxd)
            *maxd = dist[cur];
        x = cur % W;
        y = cur / W;
        if (x > 0)
        {
            nxt = cur - 1;
            if (!wall[nxt] && dist[nxt] < 0)
            {
                dist[nxt] = dist[cur] + 1;
                q[tail] = nxt;
                tail++;
            }
        }
        if (x < W - 1)
        {
            nxt = cur + 1;
            if (!wall[nxt] && dist[nxt] < 0)
            {
                dist[nxt] = dist[cur] + 1;
                q[tail] = nxt;
                tail++;
            }
        }
        if (y > 0)
        {
            nxt = cur - W;
            if (!wall[nxt] && dist[nxt] < 0)
            {
                dist[nxt] = dist[cur] + 1;
                q[tail] = nxt;
                tail++;
            }
        }
        if (y < H - 1)
        {
            nxt = cur + W;
            if (!wall[nxt] && dist[nxt] < 0)
            {
                dist[nxt] = dist[cur] + 1;
                q[tail] = nxt;
                tail++;
            }
        }
    }
    return reach;
}
int main()
{
    int i, r, md, run, starts;
    long long total, mdsum;
    rng = 1234;
    for (i = 0; i < W * H; i++)
        wall[i] = (rnd() % 100) < 25;
    total = 0;
    mdsum = 0;
    starts = 0;
    for (i = 0; i < W * H && starts < 6; i = i + 997)
    {
        if (!wall[i])
        {
            r = bfs(i, &md);
            total += r;
            mdsum += md;
            starts++;
        }
    }
    printf("%d %lld %lld\n", starts, total, mdsum);
    return 0;
}
