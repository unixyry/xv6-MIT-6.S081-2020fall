#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N 34

void func(int* p)
{
    int recv[35] = {0};
    int idx = 0;
    int p_new[2] = {0};

    while (read(p[0], &recv[idx], sizeof(recv[idx])) > 0)
    {
        if (idx == 0)     // 第一个数, 直接打印
        {
            printf("prime %d\n", recv[idx]);
        }
        else if (recv[idx] % recv[0] != 0)    // 不是第一个数的倍数
        {
            if (idx == 1)    // 第二个合法的数, 继续传递
            {
                int pid = 0;

                // 创建新的管道, 传递给子进程
                pipe(p_new);
                pid = fork();
                if (pid < 0)
                {
                    printf("[prime]Fork failed!\n");
                    exit(1);
                }
                else if (pid == 0)  // 子进程
                {
                    close(p_new[1]);
                    func(p_new);
                    close(p_new[0]);
                    wait(0);
                    exit(0);
                }
                else   // 父进程
                {
                    write(p_new[1], &recv[idx], sizeof(recv[idx]));
                }
            }
            else    // 已经fork, 父进程只需要传递
            {     
                if (recv[idx] % recv[0] != 0)
                {
                    write(p_new[1], &recv[idx], sizeof(recv[idx]));
                }
            }
        }
        else    // 是第一个数的倍数, 跳过
        {
            continue;
        }
        
        idx++;
    }

    close(p[0]);
    close(p_new[1]);
    wait(0);
    exit(0);
}

int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        printf("[prime]Usage: prime\n");
    }

    int vec[N] = {0};
    int i = 0;

    for (i = 0; i < sizeof(vec)/sizeof(vec[0]); i++)
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
        close(p[1]);
        func(p);
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