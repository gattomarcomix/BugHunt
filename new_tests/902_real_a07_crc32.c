#include <stdio.h>
#define N 20000
unsigned int table[256];
int buf[N];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
void make_table()
{
    int n, k;
    unsigned int c;
    for (n = 0; n < 256; n++)
    {
        c = n;
        for (k = 0; k < 8; k++)
        {
            if (c & 1)
                c = 0xEDB88320 ^ (c >> 1);
            else
                c = c >> 1;
        }
        table[n] = c;
    }
}
unsigned int crc32(int *p, int n)
{
    int i;
    unsigned int c;
    c = 0xFFFFFFFF;
    for (i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}
int main()
{
    int i;
    unsigned int r1, r2;
    rng = 31337;
    for (i = 0; i < N; i++)
        buf[i] = rnd() & 255;
    make_table();
    r1 = crc32(buf, N);
    r2 = crc32(buf, N / 2);
    printf("%u %u\n", r1, r2);
    return 0;
}
