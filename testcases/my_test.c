#include <stdio.h>

int main()
{
    int *p,q;
    printf("%d", *p);
    p = &q;
    printf("%d", *p);
}