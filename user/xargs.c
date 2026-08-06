#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/param.h"

#define N 128

int main(int argc, char *argv[])
{
    char* xargs_argv[MAXARG] = {0};
    char buf[N] = {0};
    int idx = 0;
    char* p = buf;

    for (; idx < argc-1; idx++)
        xargs_argv[idx] = argv[idx+1];


    // gets读到'\n'为止，遇到EOF('\0')也会停止
    gets(p, N);
    while ((*p) != '\0')
    {
        xargs_argv[idx++] = p;
        // find传过来的字符串最后一个字符是'\n',替换成'\0'
        p = p + strlen(p)-1;
        (*p) = '\0';
        p++;
        gets(p, N);
    }
    xargs_argv[idx] = 0;

    if (fork() == 0)
    {
        exec(argv[1], xargs_argv);
        fprintf(2, "xargs: exec %s failed\n", argv[1]);
        exit(1);
    }
    else
    {
        wait(0);
    }

    exit(0);
}