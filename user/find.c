#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char* get_filename(char* path, char* buf)
{
    char *p;

    // 从路径名下最后一个'/'处取出文件名
    for(p=path+strlen(path); p >= path && *p != '/'; p--)
        ;
    p++;

    strcpy(buf, p);
    return buf;
}

void func(char* direction, char* filename)
{
    char buf[32], *p;
    int fd;
    struct dirent de;
    struct stat st;

    fd = open(direction, O_RDONLY);
    if (fd < 0)
    {
        fprintf(2, "find: cannot open [%s]\n", direction);
        return;
    }

    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat [%s]\n", direction);
        close(fd);
        return;
    }

    switch(st.type){
        case T_FILE:
        {
            if (strcmp(get_filename(direction, buf), filename) == 0)
                printf("%s\n", direction);
            break;
        }

        case T_DIR:
        {
            char path[512] = {0};
            int path_len = 0;
            strcpy(path, direction);
            
            // 当不是"/"的文件夹时，添加"/"作为路径分隔符
            if (strcmp(path, "/") != 0)
            {
                p = path + strlen(path);
                *(p++) = '/';
                *(p) = '\0';
            }
            
            path_len = strlen(path);
            // 继续递归文件下的每一个对象
            while (read(fd, &de, sizeof(de)) > 0)
            {
                if (de.inum == 0)
                    continue;
                // "."和".."是当前目录和上级目录，跳过
                if (strcmp(".", de.name) == 0 || strcmp("..", de.name) == 0)
                    continue;
                // 拼接路径
                p = path + path_len;
                memmove(p, de.name, strlen(de.name));
                p = p + strlen(de.name);
                *(p) = '\0';
                func(path, filename);
            }
            break;
        }

        default:
            break;
    }

    close(fd);
    return ;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("[find]Usage: find <directory> <filename>\n");
        exit(1);
    }

    func(argv[1], argv[2]);

    exit(0);
}