#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if (argc <= 1 || argc > 2)
    {
        printf("[sleep]Usage: sleep <seconds>\n");
    }
    else
    {
        int seconds = atoi(argv[1]);
        printf("[sleep]Sleeping for %d seconds...\n", seconds);
        sleep(seconds*10);
    }

    exit(0);
}