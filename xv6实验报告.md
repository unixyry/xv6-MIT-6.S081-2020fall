# xv6 实验报告

## Lab 1：Xv6 and Unix utilities

### sleep

[Description]

1. 新增 sleep 的 shell 指令，通过在终端输入 `sleep <seconds>` 让进程休眠，并提示 `[sleep]Sleeping for %d seconds...`
2. 当输入格式不正确时提示 `[sleep]Usage: sleep <seconds>`
3. 使用系统调用 sleep 完成
4. Makefile 中新增 sleep.c 的编译

### pingpong

[Description]

1. 开启两个 pipe 文件，一个父进程写子进程读，一个子进程写父进程读
2. 通过 fork 函数生成子进程，并与父进程共享 pipe 文件

### primes

[Description]

1. 根进程依次从管道写端传入 2, 3, 4, ..., 35, 0
2. 其中 0 代表输入结束
3. fork 出的子进程通过读端接收，直到 0 结束
4. 子进程判断输入是否为 0，结束 read；输入是否是第一个非 0 参数，进行 printf；输入如果能被第一个输入整除，则接收序号 i 不递增；输入如果不能被第一个输入整除，则进一步判断接收序号是否为 1，若为 1 则 fork 出子进程递归处理，接收序号不为 1，则不用 fork 直接向子进程传递
5. 实现过程中，子进程的 `while(read() > 0)` 在父进程关闭管道写端后仍然阻塞。因为 fork 的子进程使用的管道是从父进程复制过来的，父进程的管道写端关闭，但是子进程中的管道写端未关闭，子进程的管道读端一直在等待子进程中的管道写端。
6. 子进程 fork 新子进程前，创建新的管道文件，它们之间用新的管道通信，这样子进程就可以把从父进程复制过来的管道写端关掉了，同时也不需要 0 作为结尾。

### find

[Description]

通过递归的方式遍历文件夹。

注意：

1. 递归函数中的数组不要开太大，递归太深容易栈溢出
2. 不再使用的 fd 需要及时释放，xv6 的 fd 有限制，达到上限后就会 open 失败
3. `.` 和 `..` 是当前目录和上级目录，无需处理，直接跳过

### xargs

[Description]

管道已将标准输入输出重定位，直接 gets 即可。

注意以下几点：

1. gets 不会主动移动指针，需要手动移动
2. find 传过来的字符串最后一个字符是 `\n`，需要处理
3. 执行 exec 系统调用时，传入的参数列表需要带上完整指令

## Lab 2：System calls

### trace

[Description]

通过 `trace <mask> <command>` 可以追踪 command 执行过程中所希望跟踪的系统调用，mask 表示希望跟踪的系统调用号（kernel/syscall.h 定义）。

1. 在 usys.pl 的系统调用跳转中新增 trace
2. 进程管理结构体 proc（kernel/proc.h）中新增 mask 字段，用于记录进程需跟踪的系统调用
3. 新增系统调用方法 trace（kernel/sysproc.c），修改进程管理结构体 proc 中的 mask 字段
4. 在系统调用返回时（kernel/syscall.c），统计结果并打印

### sysinfo

[Description]

实现 sysinfo 系统调用，完成对系统信息的收集。

1. 定义 get_freemem，通过统计空闲链表 freelist 的长度，计算 free memory
2. 定义 get_proc_num，通过统计进程数组 proc 中状态不为 UNUSED 的进程总数，计算 proc number
3. 通过 copyout，将内核态获取的信息拷贝给用户态指定的虚拟内存地址

## Lab 3：Page tables

### Print a page table

[Description]

通过递归的方式，DFS 三级页表中的有效页表项。

- `pte & PTE_V == 1` 表示页表项有映射
- `(pte & (PTE_R | PTE_W | PTE_X)) == 0` 表示页表项为根页表和二级页表，需要递归到三级页表

1. Print a page table 实验中，vmprint 的输出中，page 0 包含什么？page 2 中是什么？在用户模式下运行时，进程是否可以读取/写入 page 1 映射的内存？

答：page 0 包含 `init` 的执行代码和全局变量等数据，page 1 是进程用户态栈区的 guard page，在用户态运行时不可读写 page 1 映射的内存，page 2 是进程用户态的栈区。补充：page 511 包含的是 trampoline，page 510 包含的是 trapframe。

### A kernel page table per process

[Description]

1. 每个进程单独使用一张内核页表

   UART0、VIRTIO0、CLINT、PLIC、KERNBASE、etext 的虚拟地址和物理地址一致。

   TRAMPOLINE 映射关系为 `(MAXVA - PGSIZE) -> trampoline`，物理地址 trampoline 由 kernel.ld 在链接时确定。

   U_KSTACK 映射关系为 `(TRAPFRAME - 2*PGSIZE) -> 内核页表 kernel_pagetable 各进程内核栈分配到的物理页（kernel/proc.c/procinit()）`。

2. 内核态执行时，页表可能使用 kernel_pagetable，也可能使用进程的内核页表，因此 kvmpa 不再指定使用 kernel_pagetable 获取内核态物理页，而是从 satp 寄存器获取。

3. scheduler() 函数中，swtch 执行进程切换前，要在 satp 寄存器中切换新进程的内核页表。需要注意的是，swtch.S 中先将当前的 CPU 上下文（寄存器 ra、sp、s0-s11）存到全局变量 `cpus[当前cpu]->context` 结构体中，然后再将新进程 p 的上下文存到寄存器（ra、sp、s0-s11）中，其中 ra 指示 CPU 接下来执行的进程 p 的程序（swtch.S 最后 ret，pc = ra，更新指令计数器）。因此 swtch.S 的 ret 执行后，CPU 从新进程 p 的 ra 开始执行，而不是直接返回到 scheduler() 中。而当再次发生上下文切换时（sched() 执行时），全局变量 `cpus[当前cpu]->context` 结构体中的上下文被切换到寄存器中时，才会继续从 scheduler() 中的 swtch 后开始执行。注意，此时进程 p 已不再被当前 CPU（假设 CPU 0）执行，CPU 0 在执行内核逻辑时，CPU 1 释放进程 p，进程 p 的内核页表也被释放，如果在 scheduler() 中的 swtch 后不及时切换 satp 为 kernel_pagetable，CPU 0 执行内核代码就会因为地址映射失败而 panic。

[Description]

移出全局内核页表中进程内核栈部分，交由每个进程的内核页表处理。

### Simplify copyin/copyinstr

[Description]

将进程用户页表（除 trampoline 和 trapframe 外）的地址映射关系同步到进程内核页表。

1. 根据实验描述的提示，用户页表的虚拟地址最大值不超过 PLIC，防止对内核页表产生覆盖
2. 创建和释放内核页表时，去掉 CLINT 的映射和解除映射，防止和用户映射区产生重复释放
3. 记录并更新内核页表有多少有效的用户映射区（k_sz），以此准确释放内核叶子页表项，防止释放内核页表时，叶子页表项未清除导致 `freewalk` 的 `panic: leaf`
4. 用户栈下方的 guard page 映射关系直接保留，让内核态依然可以访问这段非法地址，防止因内核无法访问导致内核崩溃
5. 内核态下需要的叶子页表项标志位没有 PTE_U，在映射时需要转换，防止因内核无法访问导致内核崩溃

2. 解释为什么在 copyin_new() 中需要第三个测试 `srcva + len < srcva`：给出 srcva 和 len 值的例子，这样的值将使前两个测试为假，但是第三个测试为真。

答：数值溢出，srcva 和 len 都是 uint64。当它们的和超过 UINT64_MAX（即 2^64 - 1）时，会按模 2^64 回绕成一个较小的数。

## Lab 4：Traps

### RISC-V assembly

1. 哪些寄存器保存函数的参数？例如，在 main 对 printf 的调用中，哪个寄存器保存 13？

寄存器 a0-a7 保存函数的参数。寄存器按顺序依次保存函数参数，在 main 对 printf 的调用中，a2 保存 13。

2. main 的汇编代码中对函数 f 的调用在哪里？对 g 的调用在哪里？

对函数 f 和函数 g 的调用都被编译器优化了，在编译时进行了计算并按常量进行了替换。

3. printf 函数位于哪个地址？

从 call.asm 可以看出，printf 在 650。

4. 在 main 中 printf 的 jalr 之后的寄存器 ra 中有什么值？

jalr 执行后的寄存器 ra 中存储的是 printf 下一条指令的地址。

5. 运行以下代码。`unsigned int i = 0x00646c72; printf("H%x Wo%s", 57616, &i);` 程序的输出是什么？

程序输出 `HE110 World`，57616 转换成 16 进制就是 E110。因为 RISC-V 采用小端存储，所以从低地址到高地址依次是 72 6c 64 00，`%s` 输出的 ASCII 码依次是 rld。

6. 接上一问，如果 RISC-V 是大端存储，为了得到相同的输出，你会把 i 设置成什么？是否需要将 57616 更改为其他值？

i 需要设置成 `0x726c6400`，57616 不需要更改。

7. 在下面的代码中，`y=` 之后将打印什么（答案不是一个特定的值）？为什么会发生这种情况？`printf("x=%d y=%d", 3);`

`y=` 后面打印随机值。因为 printf 打印 `y=` 后面的值需要读取 a2 寄存器的值，但是没有赋值，所以是 a2 寄存器上一次的值。

### Backtrace

[Description]

从当前栈帧 fp 开始，逐个输出函数调用的 ra。

1. 通过 r_fp() 函数，从 s0 寄存器中读取当前栈帧的首地址 fp
2. 被调用函数的当前栈帧中，`fp-8` 的位置存储的是被调用函数返回并继续执行调用函数的地址 ra，`fp-16` 的位置存储的是调用函数的前一个栈帧的首地址
3. 每个进程的内核栈被分配了一个 PAGESIZE 的大小，栈是从高到低增长，因此最顶层的栈帧应该在内核栈对应的虚拟页内，以小于 PGROUNDUP(fp) 为停止条件

### Alarm

[Description]

时钟信号由 QEMU 模拟，时钟信号发出后中断进程并陷入内核态，中断时进程在用户态则走 uservec 逻辑，在内核态则走 kernelvec 逻辑。

内核通过 devintr() 的返回值判断中断来源于硬件设备还是时钟信号，如果是时钟信号则执行时间片切换逻辑，让出当前进程的 CPU 并切换另一个就绪态进程。

1. 实现 sigalarm() 和 sigreturn() 系统调用，sigalarm() 负责配置进程 alarm 参数，sigreturn() 负责返回到进程被 alarm 中断的上下文继续执行
2. 在 proc 结构体中新增四个变量：ticks 表示间隔，为 0 时表示不开启 alarm；ticks_remain 表示剩余多少个时钟信号触发 alarm_handler() 函数；alarm_handler 是中断触发的回调函数；trapframe_t 用于在 alarm 中断时保存进程中断信息，在 sigreturn() 时通过 trapframe_t 复原 trapframe
3. ticks_remain 在 alarm_handler() 恢复有两个作用：其一可以让 alarm 中断持续被触发，直到进程结束或主动关闭 alarm；其二是避免在 alarm_handler() 还在处理时再次发生中断，产生 alarm 嵌套

## Lab 5：Lazy allocation

### Lazy allocation

[Description]

实现懒分配，在进程调用 sbrk() 尝试分配更多内存时，仅增长进程的堆区大小记录值 proc.sz，并不实际分配物理页，也不建立页表映射关系，当发生缺页中断时，再进行物理页的分配和页表映射。

1. 当 sbrk() 收缩内存时，仍及时回收内存，防止 proc.sz 更新但是遗漏释放，导致内存泄漏以及不必要的 OOM
2. 当进程在用户态尝试在新分配的区域进行 load/save 操作时，页表中若没有有效的地址映射，则触发缺页中断，在 usertrap() 中通过 scause 寄存器的取值可以区分是否是缺页中断（load 为 13，save 为 15）
3. 在进行懒分配之前，需要对虚拟地址进行判断，以下几种情况是非法的访问，不能进行懒分配：虚拟地址超过进程申请的堆区范围、虚拟地址值溢出，以及虚拟地址是 guard page
4. 针对 uvmunmap() 在释放叶子 PTE 和物理页时，当 walk 返回 PTE 为 0 或没有 PTE_V 标志时，是因为懒分配，所以不再 panic
5. uvmcopy() 进行拷贝时，同 4
6. 当在内核进行 copyin/copyout 操作时，内核先遍历进程用户页表，找到虚拟地址 va0 对应物理地址 pa0，再进行读写，此时不会触发缺页中断，所以要对 walkaddr() 的返回值做特别判断，若 pa0 == 0 且 va0 是合法地址，则进行懒分配

## Lab 6：Copy-on-Write

### Implement copy-on-write

[Description]

在 fork() 执行 uvmcopy() 时，子进程与父进程共享父进程的物理页（除 trapframe 外），同时将父子进程的叶子 PTE 中的 PTE_W 标记去掉，物理页只读，当进程发生写操作产生缺页中断时，再分配并拷贝新的物理页，更新页表。

1. 物理页的分配需要引用计数，因为在 cow_kfree 时，当物理页没有被任何进程引用时才进行释放。为实现物理页的引用计数，维护了一个足够长的数组，对物理地址进行转换以完成下标的唯一映射
2. fork() 后，父子进程的叶子 PTE 都没有 PTE_W 权限，当需要进行 save 操作时，会触发缺页中断，scause 寄存器为 15，缺页中断地址在 stval 寄存器中，usertrap() 对此进行 copy_on_write 处理
3. copy_on_write() 就是分配并拷贝新的物理页，然后更新进程的叶子 PTE 的地址映射和 PTE_W 标志
4. copyout() 在内核态通过用户态页表找到虚拟地址映射的物理地址后，将内容从内核态拷贝到用户态，不会触发缺页中断，因此单独对叶子 PTE 进行判断和 COW 处理
5. 为方便处理：一是用数组管理引用计数，但是应该有更高效利用地址空间的数据结构；二是所有的物理页分配和释放都采用 cow_kalloc() 和 cow_free()，可以通过 PTE_C 标志位辅助判断是否是因 fork() 产生的只读页，而不需要都用 cow_kalloc() 和 cow_free() 进行物理页管理，尽管不会带来什么时间上的额外消耗

## Lab 7：Multithreading

### Uthread: switching between threads

[Description]

在用户态实现线程切换。

1. 实现 uthread_switch，保存旧线程的寄存器（返回地址 ra、线程栈顶指针 sp、线程栈帧指针 s0、局部变量寄存器 s1-s11），加载新线程的寄存器
2. 在创建线程时，手动设置 ra 为 thread_a，设置 sp 为 `all_thread[i].stack+STACK_SIZE`。sp 为栈顶指针，栈从高向低增长，因此手动设置时需要加 STACK_SIZE
3. 不需要保存和恢复其他寄存器，因为编译器不会在非栈帧区或非 s 寄存器中保存会被线程调度破坏的局部变量。这与中断和系统调用不同，因为中断可能发生在任意一处，而系统调用则涉及进程切换，所以中断和系统调用需要保存完整的 trapframe

### Using threads

[Description]

ph.c 中，多线程在 table 上通过头插法更新哈希桶下的链表，属于竞态资源，竞态分析如下，加锁后保证多线程正常进行。

q：为什么两个线程都丢失了键，而不是一个线程？确定可能导致键丢失的具有两个线程的事件序列。

a：table 是竞态资源，加入新的 key 时，是通过头插法加入新的元素。当线程 1 拿到链表头 a 还没有更新完成时，被调度到线程 2 执行。因为没有加锁，线程 2 也拿到了链表头 a，并更新此时链表头为 b。这时再调度回线程 1，线程 1 仍在旧的链表头 a 上通过头插法更新，线程 1 的更新导致线程 2 插入的 b 丢失。

### Barrier

[Description]

主要有两个注意点，其一是所有线程抵达后才对 bstate.round 更新一次，其二是所有线程离开后才开始下一轮。实现思路是通过 bstate.nthread 变量进行抵达和离开的同步。

1. 每当一个线程开始执行 barrier() 时，将 bstate.nthread 计数加一，若 `bstate.nthread != nthread`，则在条件变量 barrier_cond 上等待最后一个线程抵达并进行 broadcast
2. 最后一个抵达的线程除了负责唤醒其他线程，还负责更新 bstate.round
3. 进程在离开 barrier() 前，先将 bstate.nthread 计数减一，若 `bstate.nthread != 0`，则在条件变量 barrier_cond 上等待最后一个线程离开并进行 broadcast

## Lab 8：Locks

### Memory allocator

[Description]

为避免多 CPU 竞争单个 freelist 而在自旋锁上占用过多 CPU 资源，为每个 CPU 分配单独的 freelist，每个 CPU 优先使用自己的 freelist，当为空时，去其他 CPU 的 freelist 抢夺。

1. 初始时，freerange 中每个 kfree 将物理页挂载到当前 CPU 的 freelist 上
2. kfree 时，也是将需要释放的物理页挂在当前 CPU 的 freelist 上
3. kalloc 时，优先使用自己的 freelist，当自己的 freelist 为空时，再获取其他 CPU 的锁并从非空的 freelist 上拿取一个物理页

当两个 CPU 核心上同时出现 freelist 不足时，可能会出现 ABBA 锁，即拿着自己的锁的同时请求对方的锁，相互等待导致死锁。

考虑到从别的 freelist 上借用物理页时并不需要对自己的 freelist 做维护，因此在请求其他的锁前先释放自己的锁。

### Buffer cache

[Prototype]

1. 对块号 blockno 取余完成哈希映射，在 bcache.buf 上通过固定的数组下标偏移进行哈希映射，但是当哈希桶数量过多时，同一个桶中可用的空闲 buf 很少，极易触发 no buff 崩溃（哈希桶个数为 3 时可以通过 make grade）
2. 延续 1 的思路，当一个哈希桶没有空闲 buf 时，从相邻的哈希桶开始借用空闲 buf，但是锁不方便管理。不能冒险在持有一个哈希桶 a 的自旋锁时请求另一个哈希桶 b 的自旋锁，多线程下会死锁；但是若是在请求另一个哈希桶前释放原本映射的哈希桶，多线程会导致同一个 blockno 被映射到多个 buf 中，会触发 double free
3. 维护若干个哈希桶，它们初始为空，再额外维护一个 freelist，空闲的 buf 都在 freelist 上，当哈希桶需要时再从 freelist 拿取，使用完毕再归还，但是无法通过 bcachetest 测试，因为 freelist 频繁被竞态使用

[Description]

综合以上三个思路，对块号 blockno 取余完成哈希映射，维护哈希桶 bdgt 用于维护正在使用的 buf，哈希空桶 bdgt_free 维护建立过块号映射的空闲 buf，空闲链表 freelist 记录尚未被使用过的 buf。

1. 在初始时，bdgt 和 bdgt_free 都为空，所有 buf 都在 freelist 上
2. 当进行 bget() 时，按以下顺序寻找 buf：一是在 bdgt 上寻找正在使用 blockno 的 buf；二是在 bdgt_free 上寻找映射过 blockno 的空闲 buf，转移到 bdgt 上；三是在 bdgt_free 上取 LRU 的空闲 buf，转移到 bdgt 上；四是在 freelist 取新 buf，转移到 bdgt 上；五是从相邻哈希空桶取 LRU 的空闲 buf，转移到 blockno 映射到的 bdgt 上
3. 进行 bget() 时，全程对 blockno 映射到的 bdgt 上锁，以防止同一个 blockno 被映射到多个 buf 中。对哈希空桶和 freelist 操作时分别进行上锁，因为将哈希桶和哈希空桶拆分，因此不会出现死锁
4. 进行 brelse() 时，若引用数为 0，则将 buf 归还到 blockno 映射到的 bdgt_free 上，这样可以降低对 freelist 的竞态访问

## Lab 9：File system

### Large files

[Description]

在 inode 中，将一个直接映射的 addr 改为二级映射，思路同一级映射。

[Undo]

尽管修改 NDIRECT（从 12 改为 11），但是在 mkfs 中每个文件可用数据块只相对少了一个，并不会产生影响，因此没有对 mkfs.c 做额外修改，make 出来的镜像不会有问题。

### Symbolic links

[Description]

硬链接是与文件共享同一个 inode，增加 inode 的链接数量。软件链接是新建一个 inode，但是 inode 指向的数据块中存储的是目标文件的文件路径。

1. 在建立软链接时，将目标文件的文件路径写入新建 inode 的数据块中，不需要关注目标文件是否具体存在，也不会增加目标文件 inode 的引用数和链接数，同时将软链接类型计入 inode 中
2. 当要通过软链接寻找目标文件时，沿软链接路径依次寻找，直到找到非软链接文件，或达到递归上限停止
3. 当要通过软链接寻找目标文件但带有 O_NOFOLLOW 字段时，直接返回软链接 inode

## Lab 10：mmap

### mmap

[Desctiption]

用户进程通过 read/write 系统调用读取磁盘中的文件，mmap 允许将磁盘中的内容拷贝到物理页上，用户进程通过指针可以直接访问文件。

1. 内核为每个进程维护一个 mmap_info 结构体数组，其中存储了文件 inode、分配的虚拟地址等关键信息
2. 调用 mmap 时，内核仅新分配一个 mmap_info，并修改进程的堆区总大小 sz，并不实际分配物理页，也不更新页表
3. 当用户进程读写 mmap 的区域时，触发缺页中断，在 usertrap 中进行处理：通过文件 inode 将磁盘中的文件内容拷贝到 kalloc 得到的物理页中，并完成页表映射
4. munmap 会更复杂一些，但是因为 mmaptest 的测试数据没有那么严格，因此做简单处理。始终假设 munmap 的释放是从低地址到高地址的，在写回磁盘和释放页表时需要注意，mmap 是懒分配，因此页表中可能实际没有映射，需要额外做判断。当叶子节点本身不存在时，跳过这个逻辑页即可。需要额外注意，munmap 在确认整个映射区域都被释放后，应当修改进程的堆区总大小 sz，并清空 mmap_info
5. exit 时，如果进程还有未 munmap 的映射区域，需要执行 munmap（mmaptest 保证了不会在只释放了局部映射区域时退出进程）
6. fork 时，需要进行页表拷贝，但是由于 mmap 的懒分配，所以执行 uvmcopy 时直接跳过无效的 PTE

## Lab 11：Networking

### Network

[Description]

内核通过 PCIe（pci.c）主动通过 BDF（Bus:Device:Function）探测 QEMU 模拟的 e1000 网卡，并配置它的 memory access enable（DMA，即硬件通过设备控制器可以直接读写内存而不再通过 CPU）和 enable mastering（MMIO，当 CPU 读写这块内存区域时，转发给 e1000，等价于 CPU 通过读写内存的方式读写 e1000 的寄存器）。

Lab 11 需要实现 e1000 驱动程序中将网络报文从内存发送到网卡、从网卡接收到内存的两个基础功能。Intel 官方文档 8254x_GBe_SDM.pdf 有详细说明，这里总结实现 Lab 11 所需的内容。

1. e1000_transmit 负责发送网络报文到网卡。内核网络栈（net.c）将带有协议头的网络报文（mbuf）交给驱动程序，驱动程序填充描述符（tx_desc）并交给网卡。因此，e1000_transmit 最重要的任务就是从 e1000 的 tx 环中找到一个空闲的描述符，填充并交给 e1000 网卡
   1. e1000 网卡并不能解析 mbuf 结构体，必须按照 tx_desc 描述符将必要的信息告诉 e1000，addr 和 length 指示 e1000 的 DMA 应该从内存的什么地方取出多少数据
   2. tx_desc 描述符中的 cmd 设置为 `E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS`，前者用于告知网卡这个描述符含有一个网络报文的结束；后者用于让网卡处理完毕这个描述符后，将状态 E1000_TXD_STAT_DD 更新到描述符的 status
   3. e1000 网卡维护一个 tx 环，E1000_TDH 指向头部，E1000_TDT 指向尾部的下一个，即可用的描述符
   4. e1000 网卡会主动查询 E1000_TDT 指向是否发生改变，以从内存获取新报文，处理完描述符后，修改 E1000_TDT 即可通知网卡获取报文
2. e1000_recv 从内存获取报文并传给内核网络栈进一步处理（net_rx）。e1000 网卡收到报文之后，会通过 DMA 将报文放置到内存中，完成后发起中断通知内核进行处理
   1. e1000 网卡通过 DMA 将报文保存到内存的首地址和数据长度，由 rx_desc 描述符的 addr 和 length 描述，e1000_recv 负责将描述符翻译成 mbuf 结构体
   2. rx_desc 描述符中的 status 指示描述符对应的报文装填，E1000_RXD_STAT_DD 表示网卡已经处理完毕、驱动可以获取，E1000_RXD_STAT_EOP 表示这个描述符包含网络报文的结束
   3. e1000 网卡维护一个 rx 环，E1000_RDH 指向头部，E1000_RDT 指向尾部，即下一个是有接收报文的描述符
   4. e1000 网卡并不会主动更新 E1000_RDT，因此驱动处理完毕后，需要更新 E1000_RDT，让网卡在 rx 环上有新的描述符可以使用
   5. net_rx 在收到 ARP 请求报文后，会调用 e1000_transmit 发送 ARP 响应报文，并释放掉 mbuf。因此，e1000_recv 调用 net_rx 前需要释放锁，且需要将新的 mbuf 放置到 rx 环上
   6. 网卡一次中断可能收到多个网络报文，因此需要从 `E1000_RDT + 1` 开始寻找，直到出现无效的描述符

