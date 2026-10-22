#include <stdio.h>
int main()
{
    unsigned char a[8];
    unsigned char *q;
    int i;
    for (i = 0; i < 8; i++)
        a[i] = (unsigned char)(200 + i);
    q = a + 3;
    printf("%u %u %u %d\n", a[3], *q, q[1], (int)*q);
    return 0;
}
