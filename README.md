# xv6：MIT 6.S081 Fall 2020 实验记录

本仓库记录 [MIT 6.S081 Fall 2020](https://pdos.csail.mit.edu/6.S081/2020/) 的 xv6 实验代码和学习笔记。

实验代码基于课程官方的 `xv6-labs-2020` 仓库。它与教材使用的 [`mit-pdos/xv6-riscv`](https://github.com/mit-pdos/xv6-riscv) 基础源码仓库不同：前者为每个实验提供了单独的起始分支和测试脚本。

## 分支说明

- `docs`：默认分支，存放学习笔记和文档。
- `util`、`syscall`、`pgtbl`、`traps`、`lazy`、`cow`、`thread`、`lock`、`fs`、`mmap`、`net`：各实验的实现分支，顺序与课程 Lab 1～11 一致。

开始实验前，建议先阅读[官方实验说明](https://pdos.csail.mit.edu/6.S081/2020/schedule.html)。中文资料可参考 [xv6 中文文档](https://xv6.dgs.zone/)。

## 使用方法

### 1. 克隆仓库并切换实验分支

```bash
git clone git@github.com:unixyry/xv6-MIT-6.S081-2020fall.git
cd xv6-labs-2020
git branch -a
git switch util
```

将 `util` 替换为要进行的实验分支名称。

### 2. 启动 Docker 实验环境

各实验分支均提供 `docker_config` 目录，需要预先安装 Docker 和 Docker Compose v2。

```bash
cd docker_config
docker compose build                # 首次使用时构建镜像
docker compose run --rm xv6 bash    # 启动容器
```

镜像只需构建一次，切换实验分支后通常可以继续复用。如需查看本机镜像，可运行 `docker image ls -a`。

### 3. 编译、运行和测试 xv6

进入容器后，仓库根目录挂载在 `/workspace`，默认也会进入该目录：

```bash
make qemu
```

按 `Ctrl-a`，再按 `x` 可退出 QEMU。运行当前实验的测试：

```bash
make grade
```

需要 GDB 调试或完成 `net` 实验时，可以在另一个终端进入同一个容器：

```bash
cd xv6-labs-2020/docker_config
docker compose exec xv6 bash
```

使用完毕后，在容器中执行 `exit` 命令退出容器 Shell
