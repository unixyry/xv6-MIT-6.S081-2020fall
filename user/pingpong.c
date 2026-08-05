#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if (argc != 1)
    {
        printf("[pingpong]Usage: pingpong\n");
    }

    int pid = 0;
    int fa_to_son[2] = {0};     // 父进程写端, 子进程读端
    int son_to_fa[2] = {0};     // 子进程写端, 父进程读端

    pipe(fa_to_son);
    pipe(son_to_fa);

    pid = fork();

    if (pid < 0)    // fork失败
    {
        printf("[pingpong]Fork failed!\n");
        exit(1);
    }
    else if (pid == 0)  // 子进程
    {
        // 子进程的fa_to_son写端关闭, 读端等待父进程
        close(fa_to_son[1]);
        // 子进程的son_to_fa读端关闭, 写端写给父进程
        close(son_to_fa[0]);

        // 子进程等待父进程的ping
        char buf[10] = {0};
        read(fa_to_son[0], buf, sizeof(buf));
        printf("%d: received ping\n", getpid());

        write(son_to_fa[1], buf, sizeof(buf));
    }
    else    // 父进程
    {
        // 父进程的fa_to_son读端关闭, 写端写给子进程
        close(fa_to_son[0]);
        // 父进程的son_to_fa写端关闭, 读端等待子进程
        close(son_to_fa[1]);
        // 父进程写给子进程
        write(fa_to_son[1], "a", 1);


        // 父进程等待子进程的pong
        char buf[10] = {0};
        read(son_to_fa[0], buf, sizeof(buf));
        printf("%d: received pong\n", getpid());

        wait(0);
    }

    exit(0);
}