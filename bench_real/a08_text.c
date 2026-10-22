#include <stdio.h>
#define N 300000
char txt[N + 1];
int freq[26];
long long rng;
int rnd()
{
    rng = (rng * 1103515245 + 12345) % 2147483648;
    return rng / 65536;
}
int count_words(char *s, int *longest)
{
    int words, len;
    char *p;
    words = 0;
    len = 0;
    *longest = 0;
    p = s;
    while (*p)
    {
        if (*p == ' ')
        {
            if (len > 0)
            {
                words++;
                if (len > *longest)
                    *longest = len;
            }
            len = 0;
        }
        else
            len++;
        p++;
    }
    if (len > 0)
    {
        words++;
        if (len > *longest)
            *longest = len;
    }
    return words;
}
void reverse_range(char *a, char *b)
{
    char t;
    while (a < b)
    {
        t = *a;
        *a = *b;
        *b = t;
        a++;
        b--;
    }
}
void reverse_words(char *s)
{
    char *p, *start;
    p = s;
    while (*p)
    {
        while (*p == ' ')
            p++;
        start = p;
        while (*p && *p != ' ')
            p++;
        if (p > start)
            reverse_range(start, p - 1);
    }
}
int main()
{
    int i, r, words, longest, k;
    long long chk;
    rng = 2024;
    for (i = 0; i < N; i++)
    {
        r = rnd();
        if (r % 6 == 0)
            txt[i] = ' ';
        else
            txt[i] = 'a' + r % 26;
    }
    txt[N] = 0;
    words = count_words(txt, &longest);
    for (i = 0; i < N; i++)
    {
        if (txt[i] != ' ')
            freq[txt[i] - 'a']++;
    }
    reverse_words(txt);
    chk = 0;
    for (i = 0; i < N; i++)
        chk += txt[i] * (i % 13 + 1);
    k = 0;
    for (i = 1; i < 26; i++)
    {
        if (freq[i] > freq[k])
            k = i;
    }
    printf("%d %d %d %d %lld\n", words, longest, k, freq[k], chk);
    return 0;
}
