#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N 34

void func(int* p)
{
    int recv[35] = {0};
    int i = 0;

    memset(recv, 0, sizeof(recv));

    while (read(p[0], &recv[i], sizeof(recv[i])) > 0)
    {
        if (i == 0)     // 第一个数, 直接打印
        {
            if (recv[i] == 0)
            {
                break;
            }
            printf("prime %d\n", recv[i]);
        }
        else if (recv[i] % recv[0] != 0)    // 不是第一个数的倍数
        {
            if (i == 1)    // 第二个合法的数, 继续传递
            {
                int pid = 0;
                pid = fork();
                if (pid < 0)
                {
                    printf("[prime]Fork failed!\n");
                    exit(1);
                }
                else if (pid == 0)  // 子进程
                {
                    func(p);
                    close(p[1]);
                    close(p[0]);
                    wait(0);
                    exit(0);
                }
                else   // 父进程
                {
                    write(p[1], &recv[i], sizeof(recv[i]));
                }
            }
            else    // 已经fork, 父进程只需要传递
            {
                
                if (recv[i] % recv[0] != 0)
                {
                    write(p[1], &recv[i], sizeof(recv[i]));
                }
            }
        }
        else    // 是第一个数的倍数, 跳过
        {
            if (recv[i] == 0)
            {
                write(p[1], &recv[i], sizeof(recv[i]));
                break;
            }
            continue;
        }
        
        i++;
    }

    return ;
}

int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        printf("[prime]Usage: prime\n");
    }

    int vec[N+1] = {0};     // 最后一个数为0标记结束
    int i = 0;

    memset(vec, 0, sizeof(vec));

    for (i = 0; i < sizeof(vec)/sizeof(vec[0])-1; i++)
    {
        vec[i] = i + 2;
    }

    int p[2] = {0};
    pipe(p);

    int pid = 0;

    pid = fork();
    if (pid < 0)
    {
        printf("[prime]Fork failed!\n");
        exit(1);
    }
    else if (pid == 0)  // 子进程
    {
        func(p);
        close(p[1]);
        close(p[0]);
        wait(0);
        exit(0);
    }
    else    // root进程
    {
        close(p[0]);
        for (i = 0; i < sizeof(vec)/sizeof(vec[0]); i++)
        {
            write(p[1], &vec[i], sizeof(vec[i]));
        }
        close(p[1]);
        wait(0);
    }

    exit(0);
}