#include <stdio.h>
long long g;
int rnd()
{
    g = (g * 1103515245 + 12345) % 2147483648;
    return g / 65536 % 100;
}
int main()
{
    int i;
    g = 7;
    for (i = 0; i < 3; i++)
        printf("%d ", rnd());
    printf("\n");
    return 0;
}
