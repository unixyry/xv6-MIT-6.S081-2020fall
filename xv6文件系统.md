# xv6 文件系统

本文以当前仓库 `xv6-labs-2020` 的 **`fs` 分支**为准。该分支在基础 xv6 文件系统上完成了 large-files 和 symbolic-links 实验，因此已经包含二级间接块与符号链接。阅读文中的源码链接时，应先切换到 `fs` 分支，也可以用 `git show fs:<文件路径>` 直接查看对应版本。

xv6 文件系统把磁盘块组织成文件、目录和设备文件，并向用户进程提供 `open`、`read`、`write`、`link`、`symlink`、`unlink` 等统一接口。

一个常见的访问过程是：

```text
路径名
  ↓ 路径解析
inode
  ↓ open
进程文件描述符 fd
  ↓ read/write
全局打开文件对象 struct file
  ↓ readi/writei
文件逻辑块
  ↓ bmap
磁盘块号
  ↓ buffer cache / log / virtio
fs.img 中的数据
```

文件系统除了保存数据，还要解决以下问题：

- 用目录和路径名组织文件；
- 记录文件元数据、数据块位置和空闲块；
- 缓存磁盘块，减少慢速设备访问；
- 协调多个进程的并发访问；
- 通过日志保证一次文件系统操作在崩溃后不会只完成一半。

## 1. 文件系统分层

xv6 文件系统自顶向下可分为以下几层：

| 层次 | 主要职责 | 主要源码 |
| --- | --- | --- |
| 文件描述符 | 为进程提供整数句柄，记录打开方式和当前偏移量 | [file.c](../xv6-labs-2020/kernel/file.c)、[sysfile.c](../xv6-labs-2020/kernel/sysfile.c) |
| 路径名 | 从根目录或当前目录逐级查找 inode | [fs.c](../xv6-labs-2020/kernel/fs.c) 中的 `namei/namex` |
| 目录 | 保存“文件名 → inode 编号”的映射 | `dirlookup/dirlink` |
| inode | 表示文件，保存类型、大小、链接数和数据块索引 | `ialloc/ilock/readi/writei/bmap` |
| 日志 | 把若干块更新组成可恢复的事务 | [log.c](../xv6-labs-2020/kernel/log.c) |
| 缓冲区高速缓存 | 缓存磁盘块，并保证同一块只有一份内存副本 | [bio.c](../xv6-labs-2020/kernel/bio.c) |
| 磁盘驱动 | 通过 virtio 队列在内存缓冲区与虚拟磁盘间传输数据 | [virtio_disk.c](../xv6-labs-2020/kernel/virtio_disk.c) |

这些层并非完全单向。例如，日志层也通过缓冲区高速缓存读写日志块；inode 层修改 inode、位图和数据块时，则用 `log_write()` 把相应缓存块纳入当前事务。

## 2. 从 `fs.img` 到磁盘块

### 2.1 `fs.img` 如何成为 xv6 的磁盘

构建时，宿主机程序 [mkfs.c](../xv6-labs-2020/mkfs/mkfs.c) 创建 `fs.img`，写入超级块、根目录和用户程序等初始内容。Makefile 再通过以下参数把它挂到 QEMU 的 virtio 块设备上：

```make
-drive file=fs.img,if=none,format=raw,id=x0
-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
```

需要注意：**映射进内核页表的是 virtio 设备的 MMIO 寄存器窗口，不是整个磁盘内容。**

- `VIRTIO0` 对应一页设备寄存器，内核通过读写寄存器配置 virtio 队列；
- 请求描述符和数据缓冲区位于 guest 内存中，虚拟设备通过 DMA 访问它们；
- 文件系统块大小 `BSIZE` 为 1024 字节，驱动把文件系统块号换算为 512 字节扇区号；
- 完成请求后，virtio 设备触发中断，驱动唤醒等待该缓冲区的进程。

因此，内核不能像访问普通数组一样直接读取 `fs.img`，而要经过 `bread()`、缓冲区高速缓存和 virtio 驱动。

### 2.2 磁盘布局

`mkfs` 将文件系统划分为以下区域：

```text
[ boot | super | log | inode blocks | bitmap | data blocks ]
   0       1
```

| 区域 | 作用 |
| --- | --- |
| `boot` | 第 0 块，保留为传统磁盘引导块；当前 RISC-V xv6 不使用它启动 |
| `super` | 第 1 块，记录文件系统总大小及各区域的起始位置 |
| `log` | 保存事务头和待提交块的副本 |
| `inode blocks` | 连续存放磁盘 inode，即 `struct dinode` |
| `bitmap` | 每一位表示相应磁盘块是否已被占用或保留 |
| `data blocks` | 保存普通文件内容、目录项和间接索引块 |

![[图8 - xv6文件系统在磁盘上的布局.png]]

<center>图8 - xv6 文件系统在磁盘上的布局</center>

超级块结构如下：

```c title:fs.h/superblock
struct superblock {
  uint magic;      // 必须为 FSMAGIC
  uint size;       // 文件系统总块数
  uint nblocks;    // 数据块数量
  uint ninodes;    // inode 数量
  uint nlog;       // 日志区块数
  uint logstart;   // 第一个日志块
  uint inodestart; // 第一个 inode 块
  uint bmapstart;  // 第一个位图块
};
```

系统初始化文件系统时，`fsinit()` 从第 1 块读取超级块、检查 `magic`，然后初始化日志并执行崩溃恢复。

### 红字问题：磁盘引导块由谁填充，有什么作用

<font color="#c00000">磁盘引导块是由 QEMU 负责填充的吗？作用是什么？</font>

不是。当前仓库的 `mkfs` 创建 `fs.img` 时会先把所有块清零，因此第 0 块通常也是全零；QEMU 只是把该镜像作为 virtio 块设备接入，不会向其中填充引导程序。

`boot` 块是传统文件系统布局保留下来的位置。在旧式“从磁盘启动”的系统中，固件可能读取其中的引导代码，再由引导代码加载内核。但当前 RISC-V xv6 使用 QEMU 的 `-kernel kernel/kernel` 直接加载内核，`fs.img` 只承担根文件系统的角色，所以文件系统运行期间也不会读取第 0 块。

## 3. 缓冲区高速缓存

缓冲区高速缓存 `bcache` 是磁盘层与上层文件系统之间的共享缓存。每个 `struct buf` 对应一个 `(dev, blockno)`：

```c title:buf.h/buf
struct buf {
  int valid;               // data 是否已经包含有效磁盘内容
  int disk;                // 磁盘当前是否拥有该缓冲区
  uint dev;
  uint blockno;
  struct sleeplock lock;   // 保护此块的内容
  uint refcnt;
  struct buf *prev;
  struct buf *next;
  uchar data[BSIZE];
};
```

它有两个关键作用：

1. **缓存。** 若目标块已经在缓存中，`bread()` 不必再次访问磁盘。
2. **同步。** 对任意磁盘块，缓存中至多存在一份副本；调用者持有该 `buf` 的睡眠锁后才能读写内容，避免不同进程同时修改同一块。

典型读取过程如下：

```text
bread(dev, blockno)
  ↓
bget：查找已有 buf，或选择 refcnt == 0 的空闲 buf
  ↓
若 valid == 0，则 virtio_disk_rw(buf, 0) 从磁盘读入
  ↓
返回一个已加睡眠锁的 buf
  ↓
调用者访问 buf->data
  ↓
brelse：解锁并减少 refcnt
```

`brelse()` 在引用计数降为 0 时把缓冲区移到链表头部。需要回收时，`bget()` 从链表尾部寻找空闲项，因此优先复用最久未被释放的空闲块，可视为近似 LRU。日志还会用 `bpin()/bunpin()` 增减 `refcnt`，防止尚未提交的缓存块被回收。

## 4. 日志与崩溃恢复

文件系统操作通常要更新多个磁盘块。例如创建文件可能同时修改 inode、父目录数据块和位图。如果机器在中途崩溃，只写入其中一部分，文件系统结构就会不一致。xv6 用物理重做日志（physical redo log）把这些更新组织成事务。

### 4.1 从系统调用到日志事务

会修改文件系统的系统调用在操作前后调用：

```c
begin_op();
// 修改缓存中的 inode、bitmap、directory 或 data block
// 每个修改后的 buf 交给 log_write(bp)
end_op();
```

- `begin_op()` 等待正在进行的提交结束，并为当前操作预留最坏情况下所需的日志空间；
- `log_write(bp)` 把目标块号加入内存日志头，并固定已经修改的缓存块；它此时**不会立即把该块写入磁盘日志区**；
- 同一事务反复修改同一个块时，日志只记录一次该块号，这称为日志吸收（absorption）；
- `end_op()` 减少正在进行的操作数；最后一个操作结束后触发 `commit()`。

多个并发系统调用可能被合并成同一次提交，但提交期间不会接受新的文件系统操作。

### 4.2 提交顺序与提交点

`commit()` 的顺序是：

```text
1. write_log
   把修改后的 home block 缓存内容复制到磁盘日志数据块
        ↓
2. write_head
   把 n 和 home block 编号写入磁盘日志头
   —— 到这里事务才算已提交 ——
        ↓
3. install_trans
   把日志数据块复制到各自的最终 home block
        ↓
4. 清空 n，再次 write_head
   表示日志已经可以复用
```

其中第 2 步写入非零日志头是事务的**提交点**。必须先写完所有日志数据块，再公布日志头；否则恢复程序可能看见一个“已提交”事务，却找不到完整的新数据。

### 4.3 崩溃后是 REDO，不是 UNDO

重新启动时，`recover_from_log()` 读取磁盘日志头：

| 崩溃时机 | 磁盘日志头 | 恢复动作 |
| --- | --- | --- |
| 提交点之前 | `n == 0` | 忽略日志区中可能残留的数据，不修改 home blocks |
| 提交点之后、安装完成之前 | `n > 0` | 把日志块重新复制到 home blocks，即 REDO |
| 安装完成但日志头尚未清空 | `n > 0` | 再执行一次 REDO；重复写入相同内容是安全的 |
| 日志头已经清空 | `n == 0` | 无须恢复 |

这里没有 UNDO：xv6 不在日志中保存旧值，也不会把 home block 回滚到旧值。未到提交点的事务没有改写 home blocks，因此只需忽略；已到提交点的事务则用新值重做，最后清空日志头。

### 4.4 日志能保证什么

日志保证纳入同一事务的磁盘块更新，在恢复后表现为“全部生效”或“全部不生效”。但它不是通用数据库事务：

- 它没有事务隔离和用户可见的回滚接口；
- 日志大小固定；
- `filewrite()` 会把很大的 `write` 拆成多个较小事务，因此一次超大 `write` 系统调用并不保证整体原子。

## 5. inode：文件的持久化身份

目录项只保存文件名和 inode 编号；文件类型、大小、硬链接数和数据块地址保存在 inode 中。同一个文件在内存和磁盘上有两种不同表示。

### 5.1 磁盘 inode 与内存 inode

| 对象 | 存放位置 | 作用 |
| --- | --- | --- |
| `struct dinode` | `fs.img` 的 inode blocks | 持久保存文件元数据 |
| `struct inode` | 内核的 `icache[NINODE]` | 缓存某个磁盘 inode，并提供引用计数和睡眠锁 |

磁盘 inode 的结构为：

```c title:fs.h/dinode
struct dinode {
  short type;                 // 0 表示空闲；其余为目录、普通文件、设备或符号链接
  short major;
  short minor;
  short nlink;                // 指向该 inode 的目录项数量
  uint size;                  // 文件字节数
  uint addrs[NDIRECT + 1 + 1];// 11 个直接块 + 一级间接块 + 二级间接块
};
```

内存 inode 则额外包含设备号、inode 编号、引用计数、锁和有效位：

```c title:file.h/inode
struct inode {
  uint dev;
  uint inum;
  int ref;                    // 内存中持有该 inode 指针的引用数
  struct sleeplock lock;      // 保护下方从磁盘读入的字段
  int valid;

  short type;
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT + 1 + 1];
};
```

`iget()` 只在 inode cache 中查找或分配表项并增加 `ref`，不保证元数据已经读入；`ilock()` 获得睡眠锁，并在 `valid == 0` 时从磁盘加载 `dinode`。因此，访问 `type/size/addrs` 等字段前必须先调用 `ilock()`。

### 5.2 数据块索引

在 `fs` 分支中，large-files 实验把 inode 扩展为“直接块 + 一级间接块 + 二级间接块”：

```c
#define NDIRECT     11
#define NINDIRECT   (BSIZE / sizeof(uint))        // 256
#define NININDIRECT (NINDIRECT * NINDIRECT)       // 65536
#define MAXFILE     (NDIRECT + NINDIRECT + NININDIRECT)
```

所以 inode 的块索引为：

| 范围 | 定位方式 |
| --- | --- |
| 逻辑块 0～10 | `addrs[0]～addrs[10]` 直接保存 11 个数据块号 |
| 逻辑块 11～266 | `addrs[11]` 指向一个一级间接块，其中保存 256 个数据块号 |
| 逻辑块 267～65802 | `addrs[12]` 指向二级间接块；它的 256 个表项分别指向另一个间接块，每个间接块再保存 256 个数据块号 |

每个 inode 一共可以索引：

```text
11 + 256 + 256 × 256 = 65803 个数据块
```

所以理论最大文件大小为：

```text
65803 × 1024 = 67382272 字节，约 64.26 MiB
```

之所以把基础版本的 12 个直接地址减少为 11 个，是为了在 `dinode` 大小不变的情况下，腾出一个地址槽保存二级间接块号；`addrs[]` 的总槽数仍然是 13。[bigfile.c](../xv6-labs-2020/user/bigfile.c) 会持续写入并验证恰好 65803 个块。

`bmap(ip, bn)` 根据 `bn` 所在范围选择直接、一级间接或二级间接路径，并按需分配缺失的索引块和数据块。`itrunc()` 则沿相反方向遍历这些索引，释放所有数据块、第二级索引块和顶层二级索引块。`readi()/writei()` 根据字节偏移计算逻辑块号与块内偏移，再调用 `bmap()`。

### 5.3 `ref`、`nlink` 与 `file.ref`

这三个计数容易混淆：

| 字段 | 对象 | 含义 | 是否持久化 |
| --- | --- | --- | --- |
| `inode.ref` | 内存 inode | 内核中有多少引用持有该 inode cache 项，例如打开文件、当前目录或临时查找结果 | 否 |
| `inode.nlink` | inode 元数据 | 有多少目录项硬链接到该 inode | 是 |
| `file.ref` | 全局打开文件对象 | 有多少文件描述符表项引用同一个 `struct file` | 否 |

因此，`inode.ref` 不是“各进程调用 `open` 的次数”。一次 `open` 会间接持有 inode，但路径遍历、进程当前目录等也会增加 inode 引用；而 `dup` 或 `fork` 通常只增加同一个 `struct file` 的 `file.ref`，不会为每个新 fd 再创建 inode。

删除目录项时，`unlink` 先减少 `nlink`。只有当 `nlink == 0` 且最后一个内存引用被 `iput()` 释放时，xv6 才截断文件并回收它的数据块和 inode。这使已经打开但被删除的文件仍可通过原 fd 使用，直到最后一个引用关闭。

## 6. 目录与路径名

### 6.1 目录也是文件

目录具有自己的 inode；其数据区由一组定长目录项组成：

```c title:fs.h/dirent
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
```

- `inum == 0` 表示空闲目录项；
- 非零 `inum` 与 `name` 共同形成“文件名 → inode 编号”的映射；
- 文件名最长为 14 字节，恰好占满时不保证以 `\0` 结尾；
- 根目录 inode 编号固定为 `ROOTINO == 1`；
- `mkfs` 创建根目录时写入 `.` 和 `..`。

`dirlookup()` 顺序扫描目录数据，查找指定名字；`dirlink()` 检查名字不存在后，复用空目录项或追加新目录项。

### 6.2 路径解析

`namei(path)` 调用 `namex()` 逐段解析路径：

- 绝对路径从根 inode 开始；
- 相对路径从当前进程的 `cwd` 开始；
- 每取出一个路径分量，就锁定当前目录并用 `dirlookup()` 找到下一层 inode；
- `nameiparent()` 在最后一个分量之前停止，用于创建、链接和删除目录项。

例如解析 `/usr/rtm/xv6/fs.c` 时，开头的 `/` 只表示从根 inode 出发，内核随后按 `usr`、`rtm`、`xv6`、`fs.c` 逐级查找。当前实现是循环迭代，不是递归调用。

## 7. 硬链接与符号链接

### 7.1 硬链接

硬链接是在另一个目录位置新增目录项，使两个路径保存相同的 inode 编号：

```text
path A ─┐
        ├── 同一个 (dev, inum) ── 同一份元数据和数据
path B ─┘
```

`link(old, new)` 增加目标 inode 的 `nlink`，再把 `new` 写入目标目录。xv6 禁止对目录创建硬链接，避免形成目录环；`unlink` 删除一个名字并不一定立即删除文件，只有最后一个硬链接和最后一个内存引用都消失后才释放数据。

### 7.2 符号链接

符号链接拥有独立 inode，其载荷是目标**路径字符串**。解析符号链接时，内核读取该字符串，再把它当作路径继续查找；因此目标可以暂时不存在，符号链接也可能成为悬空链接。

`fs` 分支已经实现了符号链接：

- `stat.h` 新增 `T_SYMLINK`，用于区分符号链接 inode；
- `symlink(target, path)` 进入 `sys_symlink()`，为 `path` 创建符号链接 inode；
- `symlink_set()` 把 `target` 路径写入该 inode 的第一个数据块；
- `open(path, mode)` 默认调用 `symlink_get()` 继续查找目标；
- `O_NOFOLLOW` 使 `open()` 返回符号链接自身，`fstat()` 因而可以观察到 `T_SYMLINK`；
- 连续符号链接最多跟随 `LINK_LIMIT == 10` 层，超过限制时按链接环或过深链接处理并返回失败。

调用 `symlink()` 时目标可以不存在；只有以后通过普通 `open()` 跟随它时，目标路径才必须能够解析。若目标已被删除，符号链接 inode 仍然存在，但普通 `open()` 会失败。

```text
目录项 b
  ↓
T_SYMLINK inode
  ↓ 第一个数据块保存字符串 "/testsymlink/a"
namei("/testsymlink/a")
  ↓
目标 inode a
```

需要注意，这仍是教学实验实现，并不具备完整 Unix 符号链接语义：

- `namex()` 本身不会跟随符号链接，当前代码只在 `sys_open()` 中跟随最后得到的 inode，因此路径中间分量、`chdir` 和 `exec` 不会自动跟随；
- 相对目标路径由 `namei()` 从调用进程的当前工作目录解析，而完整 Unix 语义应当相对于符号链接所在目录解析；
- `symlink_set()` 直接操作第一个数据块但没有更新 `ip->size`，因此 `O_NOFOLLOW` 主要用于配合 `fstat()` 检查链接类型；该分支也没有提供读取链接载荷的 `readlink()`；
- 当前 `sys_symlink()` 在 `path` 已存在时会直接调用 `symlink_set()` 更新该 inode；标准 Unix 语义通常应返回“文件已存在”，并且不应覆盖普通文件的数据。阅读这段代码时应把它视为该分支的实现限制。

[symlinktest.c](../xv6-labs-2020/user/symlinktest.c) 覆盖了基本跟随、`O_NOFOLLOW`、悬空链接、链接环、连续链接和并发创建/删除等测试场景。

### 红字问题：为什么符号链接可以跨文件系统，硬链接不能

<font color="#c00000">软链接为什么可以跨盘/跨系统，硬链接不能？</font>

准确地说，是“符号链接可以指向当前目录树中另一个已挂载文件系统里的路径”，而不是可以凭空跨越未挂载磁盘或另一台独立计算机。

- 硬链接保存的是 inode 编号，而 inode 编号只在所属文件系统或设备内有意义。xv6 的 inode 身份实际是 `(dev, inum)`；目录项却只保存 `inum`，默认继承目录所在的 `dev`。因此不能用一个文件系统中的目录项直接引用另一个设备上的 inode。当前 `sys_link()` 也明确检查 `dp->dev != ip->dev` 并拒绝跨设备链接。
- 符号链接保存的是普通路径字符串。解析这个路径时，若操作系统的命名空间包含挂载点，正常路径解析就可以从一个文件系统走入另一个文件系统；链接本身无须保存目标 inode 编号。

`fs` 分支虽然已经实现符号链接，但仍只有一个根文件系统且没有实现 `mount`，所以它可以演示“保存路径而不是 inode 编号”以及悬空链接，却不能实际演示跨文件系统解析。

## 8. 文件描述符与打开文件对象

用户进程不直接持有 inode。每个进程有一个文件描述符表：

```c
struct proc {
  // ...
  struct file *ofile[NOFILE];
  struct inode *cwd;
};
```

`fd` 只是 `ofile[]` 的数组下标。表项指向内核全局文件表 `ftable.file[NFILE]` 中的打开文件对象：

```c title:file.h/file
struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
  int ref;
  char readable;
  char writable;
  struct pipe *pipe;
  struct inode *ip;
  uint off;
  short major;
};
```

三层对象的关系是：

```text
每进程 fd 表                 全局打开文件表                 inode cache

p->ofile[fd] ──────────────> struct file ────────────────> struct inode
   数组下标                    权限、共享 off、ref             (dev, inum)
```

需要据此修正几个常见误解：

1. **属于进程的是 `ofile[]`，不是 `struct file` 数组。** `ftable.file[]` 是整个内核共享的全局表。
2. **同一个 fd 数字在不同进程中可以表示完全不同的对象。** fd 只在所属进程的表中解释。
3. **分别调用两次 `open` 通常得到两个 `struct file`。** 它们可以指向同一 inode，但各自有独立的 `off`，初始通常为 0。
4. **`dup` 和 `fork` 共享原来的 `struct file`。** 新旧 fd 的 `file.ref` 增加，并共享同一个 `off`；任一 fd 读取后，另一个 fd 看到的偏移量也会前进。
5. **xv6 没有实现 `lseek`。** 普通文件的 `off` 由 `read/write` 自动推进；设备和管道使用各自的读写逻辑。

`open` 找到或创建 inode 后，先通过 `filealloc()` 分配全局打开文件对象，再通过 `fdalloc()` 把其指针放入当前进程第一个空闲的 `ofile[]` 表项，最终把该下标返回给用户。

## 9. 一次读写如何穿过各层

### 9.1 `read(fd, buf, n)`

```text
用户态 read
  ↓ 系统调用
sys_read
  ↓ argfd：fd → p->ofile[fd]
fileread
  ↓ 检查 readable，锁定 inode
readi(ip, user_dst, off, n)
  ↓ bmap：文件逻辑块 → 磁盘块号
bread
  ↓ 缓存未命中时
virtio_disk_rw
  ↓
copyout 到用户地址，并推进共享的 file->off
```

若对象是管道或设备，`fileread()` 会按 `file.type` 分派到管道或设备驱动，而不是调用普通 inode 文件的 `readi()`。

### 9.2 `write(fd, buf, n)`

```text
用户态 write
  ↓
sys_write → filewrite
  ↓ 大写入按日志容量分块
begin_op
  ↓
锁定 inode → writei
  ↓
bmap 必要时分配数据块，并更新 bitmap、一级或二级间接块
  ↓
修改 buf->data，log_write 将块纳入事务
  ↓
iupdate 更新 inode 的 size 等元数据
  ↓
end_op → 最后一个操作触发 commit
```

写入首先修改缓冲区中的块，不是直接修改 `fs.img` 的最终位置。提交时，新内容先安全地进入磁盘日志区，再安装到 inode、位图、目录或数据块的最终位置。

## 10. 核心概念总结

| 概念 | 应记住的要点 |
| --- | --- |
| virtio | 内核映射的是设备 MMIO 寄存器，不是把磁盘文件映射成内存 |
| buffer cache | 每个磁盘块至多一份缓存；用睡眠锁串行访问块内容 |
| log | 先写日志数据，再写提交头；恢复只做 REDO，不做 UNDO |
| inode | `(dev, inum)` 标识文件；`fs` 分支使用 11 个直接块、1 个一级间接块和 1 个二级间接块 |
| directory | 特殊文件，内容是定长的 `(inum, name)` 数组 |
| hard link | 多个目录项共享同一 inode，不能跨文件系统 |
| symbolic link | `fs` 分支以 `T_SYMLINK` inode 保存目标路径，`open` 默认最多跟随 10 层 |
| fd | 每进程数组下标，指向全局 `struct file` |
| file offset | 两次独立 `open` 各自维护；`dup/fork` 产生的 fd 共享 |

阅读源码时可按以下顺序串联：

1. [mkfs.c](../xv6-labs-2020/mkfs/mkfs.c)：理解磁盘镜像如何初始化；
2. [bio.c](../xv6-labs-2020/kernel/bio.c) 与 [virtio_disk.c](../xv6-labs-2020/kernel/virtio_disk.c)：理解块如何进入内存；
3. [log.c](../xv6-labs-2020/kernel/log.c)：理解修改如何原子提交；
4. [fs.c](../xv6-labs-2020/kernel/fs.c)：理解 inode、目录和路径；
5. [file.c](../xv6-labs-2020/kernel/file.c) 与 [sysfile.c](../xv6-labs-2020/kernel/sysfile.c)：理解用户接口如何连接到下层。
