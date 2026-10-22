#include <stdio.h>
unsigned int xs;
unsigned int next_rand()
{
    xs ^= xs << 13;
    xs ^= xs >> 17;
    xs ^= xs << 5;
    return xs;
}
unsigned int fnv(unsigned int h, int b)
{
    h ^= b;
    h *= 16777619;
    return h;
}
int main()
{
    int i;
    unsigned int h, r;
    xs = 2463534242;
    h = 2166136261;
    for (i = 0; i < 60000; i++)
    {
        r = next_rand();
        h = fnv(h, r & 255);
    }
    printf("%u %u\n", xs, h);
    return 0;
}
