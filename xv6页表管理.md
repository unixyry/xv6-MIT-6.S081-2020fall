# xv6 页表管理

本文的页表和进程内核页表代码以本仓库 pgtbl 分支为准，懒分配以 lazy 分支为准，写时复制以 cow 分支为准。为了说明 pgtbl 实验为什么要改 `copyin()`，文中会先交代实验前的全局内核页表；lazy 与 cow 是**各自独立的实验分支**，不能把三个分支的函数当成同一份同时运行的代码。

## 1. 从虚拟地址走到物理页

![[图4 - xv6三级页表映射.png]]

图 4：xv6 的 Sv39 三级页表。

xv6 在 RV64 上运行，代码用 `uint64` 表示虚拟地址和 PTE。Sv39 把虚拟地址的低 39 位分成三级索引与页内偏移：

| 虚拟地址位 | 38～30 | 29～21 | 20～12 | 11～0 |
| --- | --- | --- | --- | --- |
| 用途 | 根页表索引，level 2 | 中间页表索引，level 1 | 末级页表索引，level 0 | 页内字节偏移 |

每级索引为 9 位，所以每张页表有 `2^9 = 512` 个 8 字节 PTE，大小为 `512 × 8 = 4096` 字节，恰好占一个 4 KiB 物理页。普通物理页也是 4 KiB，来自 12 位页内偏移；页表页恰好同样是 4 KiB，来自 PTE 的数量与长度。一个 PTE 的低 10 位是标志位，页号可用 pgtbl 分支 `kernel/riscv.h` 中的 `PTE2PA(pte) = ((pte >> 10) << 12)` 转成物理页基址。中间 PTE 设置 `PTE_V` 而不设置 `PTE_R/PTE_W/PTE_X`，指向下一级页表；叶子 PTE 带读、写或执行权限，指向数据页。

`PTE_U` 允许用户态访问该映射。xv6 的管理态没有开启 `SUM`，因此不能直接用带 `PTE_U` 的用户虚拟地址读取用户页；内核可以经自己的非用户映射访问同一物理页。若映射或权限不满足，产生的是这次**虚拟地址访问**的异常，而不是物理页本身变得不可读。

RV64 的整数寄存器和通常的指针宽度是 64 位，不意味着全部 `2^64` 个虚拟地址都能使用。Sv39 要求高位按第 38 位符号扩展；xv6 用 `MAXVA = 1L << 38`，只用低半部地址，免去高半部的符号扩展处理。

Sv39 可在 level 1 或 level 2 放置叶子 PTE，分别映射 2 MiB 或 1 GiB 的大页，虚拟、物理基址须按相应大小对齐。pgtbl 分支的 `walk()` 与 `mappages()` 只按 level 0 的 4 KiB 普通页建映射。2 MiB 是 `2^(9+12)` 字节，原文的 `2^9^12` 写法有歧义。

CPU 的 `satp` 存放地址翻译模式和根页表的物理页号。`kvminithart()` 写入 `satp` 才启用按内核页表翻译；此前翻译关闭，内核依赖链接地址与物理地址数值相同。一次没有命中 TLB 的普通三级翻译需要读三级 PTE，然后再访问目标数据；TLB 命中省去页表遍历，不省去目标数据访问。切换页表或修改可能影响当前翻译的 PTE 后，要在合适位置执行 `sfence.vma`。pgtbl 分支的 `kvminithart()` 和调度器切换 `satp` 后就执行它，以使本 hart 后续翻译服从新映射。

## 2. `walk()` 和 `mappages()` 如何建映射

pgtbl 分支 `kernel/vm.c` 的 `walk(pagetable, va, alloc)` 先检查 `va < MAXVA`，再从 level 2 走到 level 1。遇到缺失的中间页表，只有 `alloc=1` 才用 `kalloc()` 分配、清零一个页表页并写入中间 PTE：

```c
for (int level = 2; level > 0; level--) {
  pte_t *pte = &pagetable[PX(level, va)];
  if (*pte & PTE_V) {
    pagetable = (pagetable_t)PTE2PA(*pte);
  } else {
    if (!alloc || (pagetable = (pde_t*)kalloc()) == 0)
      return 0;
    memset(pagetable, 0, PGSIZE);
    *pte = PA2PTE(pagetable) | PTE_V;
  }
}
return &pagetable[PX(0, va)];
```

返回值是 level 0 **PTE 的地址**，不是被映射物理页的地址。读取叶子物理基址时用 `PTE2PA(*pte)`；建立映射则写 `*pte`。把 `PTE2PA(*pte)` 当成可解引用的页表指针，依赖内核页表对所用 RAM 的等值映射，见第 4 节。

同一文件的 `mappages()` 将 `[va, va+size)` 覆盖的页逐一处理：对齐起止虚拟页，调用 `walk(..., 1)`，确认末级 PTE 尚未有效，再写 `PA2PTE(pa) | perm | PTE_V`；每映射一页也使 `pa` 增加 `PGSIZE`。它发现重复映射会 `panic("remap")`。调用者需提供正的大小与按页对齐的物理基址；`PA2PTE()` 不保留未对齐物理地址的页内偏移。`mappages()` 只建立映射，数据物理页通常由调用者先用 `kalloc()` 取得。

## 3. 空闲物理页怎样被分配

pgtbl 分支 `kernel/kernel.ld` 从 `KERNBASE = 0x80000000` 链接内核，定义 `etext` 与 `end`：前者在包括 trampoline 在内的代码末尾，后者在只读数据、数据和 BSS 之后。`PHYSTOP = 0x88000000` 是 xv6 使用的 RAM 上界。`end` 的具体值由链接结果决定，不由 Makefile 直接给出；Makefile 只是调用使用 `kernel.ld` 的链接工具。

启动时，`main()` 先调用 `kinit()`，再创建、启用内核页表。pgtbl 分支 `kernel/kalloc.c` 的 `kinit()` 调用 `freerange(end, PHYSTOP)`，从 `PGROUNDUP(end)` 起把完整的 4 KiB 页放进 `freelist`。所以可分配整页的物理地址范围是 `[PGROUNDUP(end), PHYSTOP)`，不是从未经对齐的 `end` 直接开始。之后这个范围内有空闲页，也有已分配的用户数据页、页表页、陷阱帧页与内核栈页，不能把整个范围始终称为“空闲”。

```c
struct run {
  struct run *next;
};

char *p = (char*)PGROUNDUP((uint64)pa_start);
for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  kfree(p);
```

**`run` 有多大，链表存在哪里？** 在 RV64 上它只有一个 8 字节指针，大小为 8 字节。每个空闲物理页的**前 8 字节**临时放 `next`，整页就是一个 `run` 节点，没有另开一块内存保存链表节点。`kfree()` 检查地址合法、按页对齐，先把整页填为 `1`，再把页首转为 `struct run *`，令 `r->next = kmem.freelist`、`kmem.freelist = r`。`kalloc()` 取出链表头，令 `kmem.freelist = r->next`，解锁后把取出的页填为 `5` 并返回页首。两次改链表都由锁保护；页一经分配，页首也不再是链表节点。填充值用于帮助发现错误使用，不代表用户页已经清零；需要零页的调用者还要执行 `memset(..., 0, PGSIZE)`。

**为什么写 `char *`？** `freerange()` 中的 `p + PGSIZE` 应前进 4096 个**字节**。`char *` 的指针加法以字节为单位；若用 `struct run *`，加 4096 会前进 4096 个结构体。`uint64` 能保存地址数值，但它是整数，需自己做加法和指针转换；这里的 `char *` 更直接地表达逐页走过字节地址。`kalloc()` 写的 `memset((char*)r, 5, PGSIZE)` 只是把页首指针改成字节指针类型：本仓库 `memset()` 本来接收 `void *`，因此这个强制转换在该调用中**不是必需的**；真正逐字节填充由 `memset()` 内部完成。

“64 位操作系统”主要关联 64 位寄存器与 ABI 中的指针宽度，并非 `char *` 必须是“64 位字符地址”才可按字节递增。32 位体系结构通常用 32 位指针，单个地址空间可表达的范围更小，但 `char *` 的加法仍按字节移动。这里选指针类型，原因是指针算术的单位，与 32 位、64 位无关。

## 4. 内核、用户和物理地址的对应关系

在 pgtbl 实验前，xv6 只有一张全局 `kernel_pagetable`。pgtbl 分支的 `kvminit()` 仍创建这张全局页表：UART、VirtIO、CLINT、PLIC 的寄存器按物理地址等值映射；`[KERNBASE, etext)` 的内核代码等值映射为可读、可执行；`[etext, PHYSTOP)` 的内核数据和所用 RAM 等值映射为可读、可写。`TRAMPOLINE` 是同一 trampoline 代码物理页在高虚拟地址上的额外映射。后两段包含 `[PGROUNDUP(end), PHYSTOP)`，因此从 `freelist` 取得的物理页，在内核页表中也有数值相同的虚拟地址。

下面重画原来的内核映射图，并把用户页表、物理地址与 pgtbl 实验后的每进程内核页表放在一起。箭头表示**页表映射**；用户虚拟地址不会以原数值“存放”在物理内存里。

![[图5a - xv6页表映射关系.svg]]

<center>图 5a：用户页表、pgtbl 每进程内核页表与物理地址的映射关系</center>

图分为上下两个面板，每一列都按高地址到低地址排列，格子高度仅用于区分区域，不代表真实地址比例。实线表示普通映射，虚线表示高地址别名，点线表示虚拟地址与物理地址数值相同的等值映射。物理页分配区中的小格只表示页的用途；这些页由 `freelist` 动态分配，实际先后次序并不固定。

图 5a 下半部分的“用户区镜像映射”**只存在于 pgtbl 的每进程内核页表**；实验前的全局内核页表没有低地址用户映射。用户根页表与中间页表页也从可分配区取出，图中省略了它们。`TRAMPOLINE` 在用户和内核页表中使用相同的**虚拟地址**，指向同一代码物理页，使陷阱入口在切换页表前后仍能执行。用户页表的 `TRAPFRAME` 指向该进程自己的陷阱帧物理页。两处高地址用户页表映射都不带 `PTE_U`。

**实验前的 `copyin()` 为什么能用物理地址作指针？** 它先通过用户页表执行 `walkaddr()`，把低地址用户虚拟指针翻译成物理页基址，检查 `PTE_U`；随后在内核态调用 `memmove()`，把这个物理基址加页内偏移作为指针。此时 CPU 按全局内核页表翻译，该物理 RAM 区间在内核页表中是**等值映射**，所以指针的虚拟地址数值和目标物理地址数值相同。跨页复制时逐页重复。实验前内核不能直接解引用原始的低地址用户虚拟指针，因为全局内核页表没有那段映射。`copyout()` 按同样原理从内核写入用户物理页。

**pgtbl 实验改变了什么？** pgtbl 分支 `kernel/proc.c` 的 `allocproc()` 在创建用户页表之外调用 `proc_kpagetable()`，为进程建一张包含内核本体、设备与自身内核栈的内核页表。`kernel/memlayout.h` 将其内核栈虚拟基址定义为 `U_KSTACK = TRAPFRAME - 2*PGSIZE`；物理栈页由 `kalloc()` 分配。实验前按进程编号排列的全局高地址内核栈映射，与这里的 `U_KSTACK` 布局属于不同版本。调度器运行某进程时切换 `satp` 到该进程的内核页表，不运行进程时切回全局内核页表。

随后，pgtbl 分支 `kernel/vm.c` 的 `copy_uspace()` 把用户区叶子 PTE 所指的物理页也映射到进程内核页表的**同一个低虚拟地址**，并去掉内核侧的 `PTE_U`。它复制的是映射关系，不是物理页内容。因此 `copyin_new()` 可在进程内核页表下直接用用户指针做 `memmove()`，无需逐页用用户页表查物理地址；pgtbl 分支 `kernel/vm.c` 的 `copyin()` 就调用它，而 `copyout()` 仍逐页用 `walkaddr()`。用户区映射必须随 `userinit()`、`fork()`、`exec()`、`sbrk()` 同步。为避免与 PLIC 的低地址 `0x0c000000` 重叠，实验把用户区控制在 PLIC 之前；`proc_kpagetable()` 也没有给每进程内核页表再加入位于低地址、可能与用户区冲突的 CLINT 映射。

这套实现增加了 `k_pagetable` 与 `k_sz`，用 `copy_uspace()`、`kvmalloc()`、`kvmdealloc()` 等函数维护两张页表；它们是 **pgtbl 分支**的源码，不属于实验前的全局内核页表实现。

源码核对时还要留意用户栈保护页：它的用户 PTE 已无 `PTE_U`，而当前 `copy_uspace()` 只在 `PTE_U` 存在时重算内核侧权限，遇到保护页会沿用上一个循环的权限值。正确同步应对每个 PTE 单独计算 `k_flags = u_flags & ~PTE_U`，保留该页自己的读写等标志。

## 5. 第一个进程、`exec` 和堆的增长

pgtbl 分支 `kernel/proc.c` 的 `allocproc()` 从进程表找 `UNUSED` 槽位，分配 PID 和独立陷阱帧物理页。`proc_pagetable()` 调用 `uvmcreate()` 分配空的用户根页表，再把共享的 trampoline 代码物理页映射到 `TRAMPOLINE = MAXVA - PGSIZE`，把该进程的陷阱帧物理页映射到紧下方的 `TRAPFRAME`；两者都不带 `PTE_U`。`proc_kpagetable()` 则建立进程内核页表并给自身分配内核栈。新进程的内核上下文 `ra` 指向 `forkret`，`sp` 指向内核栈顶；调度器首次切换到它时，`swtch` 的 `ret` 进入 `forkret`，再经 `usertrapret()` 返回用户态。

`userinit()` 创建第一个用户进程。编入内核的 `initcode` 字节数组经 `uvminit()` 拷入**一个普通用户物理页**，将 `[0, PGSIZE)` 映射为用户可读、可写、可执行；随后 `userinit()` 设置 `p->sz = PGSIZE`、入口 `epc = 0` 和用户栈指针 `sp = PGSIZE`，并调用 `copy_uspace()` 将这一页的映射同步到进程内核页表。

**一页容得下代码和栈吗？** 对这段很小的引导程序足够。`sp` 指向页末，入栈时先向低地址移动；代码、数据和短暂使用的栈都在 `[0, PGSIZE)`，主要工作是调用 `exec("/init", ...)`，失败则退出。这里的“一个页”只指 `initcode` 的普通用户页；进程还占用陷阱帧页、页表页与内核栈页，并共享 trampoline 页。原文写的 `[0, PGSIZE]` 会把不可访问的页末地址也算入，准确区间应为半开区间 `[0, PGSIZE)`。

`exec()` 在 pgtbl 分支 `kernel/exec.c` 中另建用户页表，按 ELF 程序头的 `ph.vaddr` 与大小分配物理页、加载指令和数据，并在其后分配两页：低的一页作为用户栈保护页，高的一页作为一页用户栈。保护页的 PTE **仍有效**，只是 `uvmclear()` 清除 `PTE_U`，令用户态触碰时发生异常；它不是完全没有物理页的“无效映射”。成功后，`exec()` 才更新程序入口、用户栈指针和 `p->sz`，替换旧用户页表，并通过 `copy_uspace()` 更新该进程内核页表的用户区。第一个完整程序是文件系统中的 `/init`；`user/init.c` 是它的源码，不能把 `init.c` 自身当作 ELF 文件。ELF 段依各自虚拟地址装入，也不一定全部从 0 开始。

完整程序的用户虚拟地址大体按以下顺序排列。具体段位置和大小由 ELF 决定，空白区未映射：

```text
低地址  代码 / 全局数据 / BSS  →  用户栈保护页（PTE_U=0）  →  一页用户栈
        →  堆（sbrk 从 p->sz 向高地址增长）  →  未映射空白区
        →  TRAPFRAME（管理态专用）  →  TRAMPOLINE（管理态专用）  高地址
```

用户栈在自己的页内向低地址增长，堆向高地址增长。`TRAPFRAME`、`TRAMPOLINE` 虽在用户页表中，却不能被用户态当作堆或栈读写。基础 xv6 不会自动扩展一页用户栈。

pgtbl 分支 `growproc()` 处理 `sbrk` 的大小变化：正增长调用 `kvmalloc()`，从 `PGROUNDUP(oldsz)` 起分配并清零新物理页，同时建立用户和进程内核两侧映射；负增长调用 `kvmdealloc()`，跨过完整页边界时取消两侧叶子映射，只归还用户数据物理页，内核侧不再重复 `kfree()`。`p->sz` 记录低地址已分配区的上界，不单是“堆大小”；`k_sz` 用于维护进程内核页表中同步过的用户区。收缩时中间页表页通常保留，进程退出或替换页表时再由 `freewalk()` 递归释放页表页。`proc_free_kpagetable()` 清掉同步的用户映射与内核本体映射时使用 `do_free=0`，避免重复释放物理页；自身内核栈映射才使用 `do_free=1`，随后释放内核页表页。pgtbl 分支的 `growproc()` 试图把地址上界限制在 PLIC 前；其源码在超界分支返回 `0`（按函数约定代表成功），因此不应把这行代码描述为已可靠地拒绝超界请求。

用户态 `user/umalloc.c` 的 `malloc()` 和 `free()` 按 `Header` 单位管理循环空闲链表，头部记录下一块地址和块大小。找不到足够大的块时，`morecore()` 才调用 `sbrk()` 扩大地址空间；`free()` 把块放回用户态链表并合并相邻块，通常不立即归还物理页。于是 `malloc` 管理进程堆里的小块，`sbrk` 与内核页表管理进程地址空间和页粒度映射。

## 6. lazy 分支：按需分配堆页

lazy 分支从较早的基础页表代码单独起步，**没有沿用 pgtbl 的每进程内核页表与 `kvmalloc()`**。其 `kernel/sysproc.c` 的 `sys_sbrk()` 在正增长时只增加 `p->sz`，不立即分配物理页或建立有效 PTE；负增长仍调用 `growproc()` 释放已分配的页。这样，申请很大的堆区却只用其中少数页的进程，能把分配成本延后到第一次访问。

用户指令第一次读或写尚未映射的合法地址时，CPU 产生加载页错误 `scause=13` 或存储页错误 `scause=15`。lazy 分支 `kernel/trap.c` 的 `usertrap()` 用 `r_stval()` 取得出错虚拟地址，调用 `kernel/proc.c` 的 `lazy_alloc()`：先用 `PGROUNDDOWN()` 取页首，检查地址是否在 `p->sz` 范围内、是否碰到用户栈保护页，再调用 `kalloc()` 与 `mappages()`。若越界、碰到保护页或无可用物理页，则杀死进程。对“已向 `sbrk` 申请过的地址”与“任意未映射地址”必须区别处理。

由于 lazy 分支的 `copyin()`、`copyout()` 在内核中软件遍历用户页表后，经内核等值映射访问物理页，系统调用收到一个**合法但尚未落实**的用户地址时，不会自动触发用户态缺页异常。其 `kernel/vm.c` 在这些复制路径遇到 `walkaddr() == 0` 时尝试补建物理页和映射；`uvmcopy()` 与 `uvmunmap()` 也跳过合法的未映射空洞，避免 `fork()` 或收缩内存时把懒分配空洞当成错误。用户栈保护页仍不得被这种补建逻辑当作堆页。

**源码正确性边界：** 当前 lazy 分支的 `lazy_alloc()` 和 `copyin()/copyout()` 补建路径直接调用 `kalloc()`、`mappages()`，没有把新页清零；而 `kalloc()` 会把页填为 `5`。因此“首次访问得到零页”是正确的懒分配要求，**不是该分支这些路径现有代码的实际行为**。复制路径还应完整核验地址确实是合法懒分配堆页，避免误处理栈保护页或其他权限错误。本文描述策略时以此为界，不把源码的遗漏写成已实现的性质。

## 7. cow 分支：延后 `fork()` 的物理页复制

cow 分支同样从基础页表代码单独起步，**没有沿用 pgtbl 的每进程内核页表，也不是 lazy 分支的延续**。普通 `fork()` 的 `uvmcopy()` 为子进程逐页分配新物理页并复制内容；cow 分支 `kernel/vm.c` 的 `uvmcopy()` 则在 `[0, p->sz)` 的普通用户区让子页表先指向父页表的物理页，清除父、子叶子 PTE 的 `PTE_W`；高地址的 `TRAPFRAME` 与 `TRAMPOLINE` 不在这次复制循环中。进程第一次写共享页时，CPU 触发存储页错误 `scause=15`；cow 分支 `kernel/trap.c` 的 `usertrap()` 调用 `copy_on_write()`，为出错进程分配私有页，复制旧内容，更新其 PTE 并恢复写权限。

共享页不能在第一个进程释放映射时就放回 `freelist`。cow 分支 `kernel/kalloc.c` 另设 `cow_kalloc()`、`cow_kfree()` 和 `cow_copy()`：分配时建立引用计数，`fork()` 共享页时增加计数，解除映射或复制成私有页后减少计数，只有计数归零才把物理页还给空闲链表。内核的 `copyout()` 经物理地址写用户页，不会由用户态写保护自动触发异常，因此 cow 分支在写入前也调用 `copy_on_write()` 处理只读映射。

**源码正确性边界：** 当前 cow 分支 `uvmcopy()` 对所有用户叶子 PTE 都清除 `PTE_W`，而 `copy_on_write()` 将任何不可写叶子页都当作 COW 页，若进程存在原本只读的用户页，也会把它在第一次写错误后变成可写。基础 xv6 的 `uvmalloc()` 当前给普通用户页设置 `PTE_W`，但 COW 机制不应依赖这一权限配置。正确做法是只把**原本可写**的共享页标成 COW，保留原本只读页的权限，并在页错误及 `copyout()` 中识别专门的 COW 标志。引用计数还应只在最后一个引用消失时归还页。由此，本文关于“原本只读页仍应只读”的结论是策略正确性要求，不是这份 cow 分支已全面做到的事实。

懒分配延后**取得**物理页，COW 延后**复制已有**物理页。二者都用页表权限和页错误把工作推迟到实际访问，但还必须处理内核复制、`fork()`、取消映射及失败清理路径；只改 `usertrap()` 不足以完成任一机制。

参考：[xv6 2020 教材](https://pdos.csail.mit.edu/6.828/2020/xv6/book-riscv-rev1.pdf)、[MIT 页表实验](https://pdos.csail.mit.edu/6.828/2020/labs/pgtbl.html)、[MIT 懒分配实验](https://pdos.csail.mit.edu/6.828/2020/labs/lazy.html)、[MIT COW 实验](https://pdos.csail.mit.edu/6.828/2020/labs/cow.html)、[RISC-V 特权级规范](https://docs.riscv.org/reference/isa/v20240411/_attachments/riscv-privileged.pdf)。
