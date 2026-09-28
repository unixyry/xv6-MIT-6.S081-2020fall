# xv6 启动流程

本文以当前仓库的 `xv6-labs-2020` RISC-V 版本为准。它使用 QEMU `virt` 虚拟机，以 `-bios none -kernel kernel` 直接启动内核；因此它的启动方式与旧版 x86 xv6 的磁盘 boot block、使用 OpenSBI 的 RISC-V 系统或真实硬件并不相同。

## 1. 总体流程

从构建到 shell 出现，主线如下：

```text
Makefile 编译、链接 kernel ELF，并制作 fs.img
        ↓
宿主机启动 qemu-system-riscv64
        ↓
QEMU 将 kernel 的可加载段放入 guest RAM
        ↓
CPU 从 QEMU 提供的 0x1000 复位 ROM 开始执行
        ↓
复位 ROM 跳到 0x80000000 的 _entry（M 模式）
        ↓
entry.S 为每个 hart 设置启动栈，调用 start()
        ↓
start() 配置 M 模式 CSR、定时器和陷阱委托
        ↓ mret
main()（S 模式）初始化内存、页表、进程、设备等子系统
        ↓
userinit() 构造第一个进程 initcode，并置为 RUNNABLE
        ↓
scheduler() → swtch() → forkret() → usertrapret() → userret
        ↓ sret
initcode 在 U 模式执行 exec("/init", argv)
        ↓
同一个 PID 1 进程换成 user/init.c 对应的程序映像
        ↓
init 创建控制台，fork() 子进程，子进程 exec("sh", argv)
        ↓
shell 运行；init 留在后台回收孤儿进程并在 shell 退出后重启它
```

当前 Makefile 默认设置 `CPUS := 3`，并通过 `-smp $(CPUS)` 要求 QEMU 启动多个 hart。所以 `_entry`、`start()` 和 `main()` 都会在每个 hart 上执行；只有 hart 0 负责一次性的全局初始化，其他 hart 等待初始化完成后再进入自己的 `scheduler()`。

## 2. 内核如何被放到 `0x80000000`

### 2.1 Makefile 与链接脚本的职责

Makefile 先把 `entry.S`、内核 C 文件和其他汇编文件编译成目标文件，再用 [kernel.ld](../xv6-labs-2020/kernel/kernel.ld) 链接为 ELF 格式的 `kernel/kernel`：

```make
$K/kernel: $(OBJS) $(OBJS_KCSAN) $K/kernel.ld $U/initcode
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) $(OBJS_KCSAN)
```

链接脚本中的关键内容简化如下：

```ld
ENTRY(_entry)

SECTIONS
{
  . = 0x80000000;
  .text : {
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.*) }
  .data   : { *(.data .data.*) }
  .bss    : { *(.bss .bss.*) }
}
```

- `. = 0x80000000`：从该地址开始布置内核节，形成 ELF 可加载段的地址。
- `entry.o` 位于 `OBJS` 首位，它的 `.text` 首先进入输出 `.text`，所以 `_entry` 位于内核开头。
- `ENTRY(_entry)`：把 ELF 头中的入口符号设置为 `_entry`。

Makefile 启动 QEMU 时使用：

```make
QEMUOPTS = -machine virt -bios none -kernel $K/kernel \
           -m 128M -smp $(CPUS) -nographic
```

`-kernel` 让 QEMU 加载内核映像；`-bios none` 表示不加载 OpenSBI 等外部固件。当前内核 ELF 的加载基址、入口 `_entry` 和链接地址都被安排为 `0x80000000`，因此加载位置和开始执行位置一致。

### 红字问题：引导加载程序 `boot` 在哪里

<font color="#c00000">引导加载程序 `boot` 在哪里？</font>

当前 RISC-V xv6 仓库中没有一个名为 `boot` 的 xv6 引导程序，也没有旧版 x86 xv6 那样从磁盘扇区加载内核的 boot block。QEMU `virt` 平台在物理地址 `0x1000` 提供一小段复位 ROM 代码；CPU 复位后先执行它，再跳转到 QEMU 已加载的内核。

`-bios none` 只是取消独立 BIOS/固件映像，并不意味着 CPU 复位后直接凭空出现在 `_entry`。QEMU 仍会生成必要的复位向量。仓库的 [memlayout.h](../xv6-labs-2020/kernel/memlayout.h) 也明确记录了 `0x1000` 是 QEMU boot ROM、`0x80000000` 是 ROM 跳转目标和内核加载位置。QEMU 侧的实现可参见 [RISC-V boot helper](https://gitlab.com/qemu-project/qemu/-/blob/master/hw/riscv/boot.c) 与 [`virt` 机器初始化](https://gitlab.com/qemu-project/qemu/-/blob/master/hw/riscv/virt.c)。

### 红字问题：如何保证从 `entry.S` 开始

<font color="#c00000">如何保证内核加载后 CPU 从 `entry.S` 开始？是通过 `kernel.ld` 和 Makefile 吗？</font>

这是构建工具和 QEMU 共同完成的，而不是某一个文件单独完成：

1. `entry.S` 定义 `_entry`，编译后得到 `entry.o`。
2. Makefile 把 `entry.o` 放在内核目标文件列表首位，并指定 `-T kernel/kernel.ld`。
3. `kernel.ld` 从 `0x80000000` 布置 `.text`，并用 `ENTRY(_entry)` 记录 ELF 入口。
4. Makefile 把生成的 ELF 作为 QEMU 的 `-kernel` 参数。
5. QEMU 依据 ELF 的可加载段地址把内核放入 guest RAM，并让复位 ROM 跳到内核启动地址；在当前映像中，该地址就是 `0x80000000` 的 `_entry`。

因此，`kernel.ld` 决定映像的地址布局和入口元数据，Makefile 决定如何链接以及如何把映像交给 QEMU，QEMU 才真正完成加载并建立复位跳转。可用 `readelf -h -l kernel/kernel` 和 `nm kernel/kernel | grep _entry` 检查最终 ELF，而不能只看源文件推断结果。

低于 `0x80000000` 的地址并非全部都是 I/O 设备；更准确地说，QEMU `virt` 的 DRAM 从 `0x80000000` 开始，较低地址包含复位 ROM、CLINT、PLIC、UART、virtio MMIO、空洞等平台区域，所以 xv6 把普通内存和内核放在 `0x80000000` 及以上。

## 3. `_entry`：建立每 hart 启动栈

[entry.S](../xv6-labs-2020/kernel/entry.S) 开始执行时处于 M 模式，分页尚未启用。它首先为当前 hart 选择一段栈，然后调用 C 函数 `start()`：

```asm
.section .text
_entry:
        la sp, stack0
        li a0, 4096
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        call start
spin:
        j spin
```

[start.c](../xv6-labs-2020/kernel/start.c) 中的定义为：

```c
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];
```

所以实际公式是：

```text
sp = stack0 + 4096 × (mhartid + 1)
```

hart 0 使用 `stack0[0 ... 4095]`，初始 `sp` 指向 `stack0 + 4096`；hart 1 使用下一页，以此类推。RISC-V 栈向低地址增长，因此空栈的 `sp` 应从各自区域的高地址开始。每个栈大小为 4096 字节，且起始定义按 16 字节对齐，满足 RISC-V ABI 的栈对齐要求。

如果 `start()` 意外返回，`call start` 后会落入 `spin` 无限循环；正常路径通过 `mret` 跳转到 `main()`，不会返回 `_entry`。

### 红字问题：为什么执行 C 代码需要栈

<font color="#c00000">为什么执行一个 C 程序需要在内存中准备栈？</font>

并非每一条 C 语句都必然访问栈，但普通 C 代码是按调用约定编译的，必须在进入前提供有效的 `sp`。函数可能用栈保存：

- 返回地址 `ra` 和被调用者保存寄存器；
- 局部变量、临时值和寄存器溢出值；
- 无法全部通过参数寄存器传递的参数；
- 嵌套函数调用所需的栈帧。

`call` 指令本身只把返回地址写入 `ra`，不会自动创建完整栈帧。这里的“函数序言”（function prologue）是编译器放在函数入口、用于建立当前函数栈帧的一小段汇编代码；它通常会减少 `sp`，再把需要保留的数据写入栈。若 `_entry` 未先设置合法的 `sp`，编译后的 `start()` 一旦使用栈就会读写未知内存。

### 红字问题：为什么只额外准备栈

<font color="#c00000">C 程序不是有代码段、数据段、堆和栈吗？为什么这里只准备栈？</font>

这里混淆了“用户进程的典型虚拟地址布局”和“内核刚启动时的 ELF 物理布局”。在 `_entry` 之前：

- QEMU 已根据内核 ELF 加载 `.text`、`.rodata`、`.data`，并为 `.bss` 保留和清零内存。
- `stack0` 本身就是内核 `.bss` 中的全局数组，并不是 `_entry` 临时向内存管理器申请出来的。
- 分页尚未启用，内核按链接地址直接访问物理地址；此时还没有用户进程的虚拟地址空间。
- xv6 内核没有传统意义上连续增长的 C `malloc` 堆。`kinit()` 稍后把链接符号 `end` 到 `PHYSTOP` 之间的物理页交给 `kalloc()` 管理。

所以代码段和数据段已经存在，早期代码只缺一个可供 C 调用约定使用的有效 `sp`。用户进程的代码、数据、用户栈和堆要到创建进程、建立用户页表后才出现。

### 红字问题：`sp` 指向栈顶有什么意义

<font color="#c00000">把 `stack0 + 4096` 加载到 `sp` 有什么意义？栈指针有什么用？</font>

对 hart 0 而言，`stack0 + 4096` 是其 4 KiB 栈区的高地址边界。函数创建栈帧时先将 `sp` 向低地址移动，再通过相对 `sp` 的偏移访问保存值；函数返回前再把 `sp` 恢复。它既划定当前栈帧位置，也让嵌套调用可以按后进先出的方式使用同一段内存。

需要区分 xv6 中的三类栈：

| 栈 | 数量 | 用途 |
| --- | --- | --- |
| `stack0` 启动栈 | 每个 hart 一段 | 运行 `_entry`、`start()`、`main()`，之后承载该 CPU 的调度器执行流 |
| 进程内核栈 | 每个进程表项一页，并带保护页 | 该进程进入内核后运行系统调用、陷阱处理和内核函数 |
| 用户栈 | 每个用户进程自己的用户地址空间中 | 用户程序的函数调用和局部数据 |

## 4. `start()`：从 M 模式切换到 S 模式

每个 hart 都执行 `start()`。其关键配置如下：

1. 将 `mstatus.MPP` 设置为 S 模式，指定 `mret` 的目标特权级。
2. 将 `main` 地址写入 `mepc`，指定 `mret` 后的 `pc`。
3. 将 `satp` 写为 0，保持地址转换为 Bare 模式。
4. 写 `medeleg`、`mideleg` 和 `sie`，配置以后在较低特权级发生的陷阱。
5. `timerinit()` 为当前 hart 配置机器模式定时器中断和 `timervec`。
6. 把 `mhartid` 保存到 `tp`，供 `cpuid()` 使用。
7. 执行 `mret`：CPU 将特权级切换为 `mstatus.MPP` 指定的 S 模式，并从 `mepc`，也就是 `main()`，继续执行。

这里的 `mret` 与普通函数 `ret` 不同：`ret` 从通用寄存器 `ra` 取得地址；`mret` 从 CSR `mepc` 取得地址并恢复特权状态。

### 红字问题：为什么此时禁用分页

<font color="#c00000">为什么 `start()` 要执行 `w_satp(0)` 禁用分页？</font>

此时内核页表尚未创建，不能启用基于页表的地址转换。内核又被链接并加载到相同的地址 `0x80000000`，所以 Bare 模式下可以直接把指令使用的地址当作物理地址。显式写 `satp = 0` 也把每个 hart 置于已知状态，而不依赖复位前环境。

hart 0 进入 `main()` 后执行：

```text
kinit() → kvminit() → kvminithart()
```

`kvminit()` 创建全局内核页表，其中内核 RAM 采用直接映射；`kvminithart()` 才把该页表写入当前 hart 的 `satp` 并刷新 TLB。其他 hart 等待 hart 0 创建好共享页表，再分别调用自己的 `kvminithart()`。由于内核地址在切换前后都可用，CPU 能连续执行。

### 红字问题：当前仍在 M 模式，如何把陷阱委托给 S 模式

<font color="#c00000">当前仍在机器模式，如何把中断和异常委托给管理模式？</font>

正因为当前处于 M 模式，`start()` 才有权限写 `medeleg` 和 `mideleg`。这两个 CSR 设置的是**未来的陷阱路由策略**：相应位被设置后，将来在 S/U 模式发生的可委托异常或中断可以直接陷入 S 模式，而不必先由 M 模式处理。它不是把当前正在执行的代码或当前已有的处理过程“移交”给 S 模式，而且 M 模式自身发生的陷阱不会向更低特权级委托。

并非所有中断都能这样委托。当前实现仍让机器定时器中断进入 M 模式的 `timervec`；`timervec` 安排下一次定时器中断，再置位 `sip.SSIP`，把它转换为 S 模式软件中断，最终由内核的 S 模式陷阱路径处理。

## 5. `main()`：多 hart 初始化与页表启用

[main.c](../xv6-labs-2020/kernel/main.c) 中，hart 0 负责一次性的全局初始化：

```text
console/printf
    ↓
kinit                    物理页分配器
    ↓
kvminit/kvminithart      创建内核页表，并在 hart 0 启用分页
    ↓
procinit                 进程表和进程内核栈地址
    ↓
trapinit/trapinithart    陷阱锁和当前 hart 的 stvec
    ↓
plicinit/plicinithart    外部中断控制器
    ↓
binit/iinit/fileinit     缓冲、inode、文件表
    ↓
virtio_disk_init         磁盘设备
    ↓
userinit                 构造第一个用户进程
```

初始化完成前，其他 hart 在 `started == 0` 上自旋。hart 0 在发布所有初始化结果后把 `started` 置为 1；内存屏障保证其他 hart 不会看到标志已更新，却仍看不到之前的共享数据写入。其他 hart 随后各自启用内核页表、安装陷阱入口并配置 PLIC，最后所有 hart 都进入 `scheduler()`，且不再从中返回。

文件系统的超级块和日志并没有在 `main()` 中调用 `fsinit()` 初始化。这些操作可能因磁盘 I/O 睡眠，必须在普通进程上下文中进行，所以第一次运行 `forkret()` 时才执行一次 `fsinit(ROOTDEV)`。

## 6. `userinit()`：构造第一个用户进程

### 6.1 `allocproc()` 准备内核侧状态

`userinit()` 首先调用 `allocproc()`。后者：

1. 找到一个 `UNUSED` 进程表项并保持其 `p->lock`。
2. 分配 PID。
3. 分配一页 `trapframe`。
4. 创建用户页表，并映射 `TRAMPOLINE` 与该进程的 `TRAPFRAME`。
5. 清空 `p->context`，设置 `p->context.ra = forkret`、`p->context.sp = p->kstack + PGSIZE`。

这里需要修正原文：`allocproc()` **没有在此时分配新的进程内核栈**。hart 0 创建内核页表时，`proc_mapstacks()` 已经为每个进程表项分配并映射了一页内核栈；`procinit()` 把相应虚拟地址记入 `p->kstack`。`allocproc()` 只是让新上下文的 `sp` 指向这页现成内核栈的顶部。

`ra = forkret` 使调度器第一次 `swtch()` 到该进程时，`swtch` 最后的 `ret` 跳到 `forkret()`。这并不表示用户代码从 `forkret` 开始；`forkret` 仍是内核态入口。

### 6.2 映射 `initcode`

随后 `userinit()` 完成：

```c
uvminit(p->pagetable, initcode, sizeof(initcode));
p->sz = PGSIZE;
p->trapframe->epc = 0;
p->trapframe->sp = PGSIZE;
p->cwd = namei("/");
p->state = RUNNABLE;
```

`uvminit()` 分配一页物理内存，将它映射到用户虚拟地址 `0`，并复制 `initcode` 字节。该页暂时同时容纳极小的代码、数据和向下增长的用户栈：

- `epc = 0`：第一次 `sret` 后从虚拟地址 0 的 `start` 开始执行。
- `sp = PGSIZE`：用户栈从第一页顶部向低地址增长。
- `RUNNABLE`：允许某个 CPU 的调度器选中它。

此时进程名是 `initcode`，但它已经是 `initproc` 指向的第一个进程，通常 PID 为 1。以后成功执行 `/init` 只会替换它的程序映像，不会创建新的 PID。

### 红字问题：`initcode` 为什么使用汇编

<font color="#c00000">为什么第一个用户程序 `initcode` 以汇编形式编写？</font>

第一个用户进程出现时还不能依赖普通用户库来启动，也必须先通过 `exec` 才能从文件系统装入完整的 `/init`。`initcode.S` 只需构造两个参数并直接执行 `ecall`，汇编可以精确控制入口地址、位置无关寻址、系统调用号和代码大小，不需要 C 启动代码、栈帧约定或 `usys.S` 包装函数。

这并不是硬件规定“第一个程序必须用汇编”；理论上也能使用经过特殊链接的极小 freestanding C 程序，但构建和运行时依赖更复杂，生成代码也不如这几十字节汇编可控。

当前 Makefile 将 [initcode.S](../xv6-labs-2020/user/initcode.S) 以 `start` 为入口、以虚拟地址 0 链接，并转换成纯二进制：

```make
$(LD) ... -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
$(OBJCOPY) -S -O binary $U/initcode.out $U/initcode
```

本仓库的 [proc.c](../xv6-labs-2020/kernel/proc.c) 以 `uchar initcode[]` 保存对应机器码，并由 `userinit()` 复制它。`initcode.S` 是这些机器码的可读源形式。需要注意，当前 Makefile 虽然生成 `user/initcode` 二进制，但不会把该文件自动嵌入内核；`proc.c` 注释中的 `od -t xC initcode` 表明，修改汇编后还需要同步更新这个字节数组。

## 7. 第一次调度与进入用户态

`userinit()` 返回后，hart 0 最终进入 `scheduler()`。第一次运行 PID 1 的完整路径是：

1. `scheduler()` 找到 `RUNNABLE` 的 `initcode`，持有其锁并改为 `RUNNING`。
2. `swtch(&c->context, &p->context)` 切换到预设上下文；载入的 `ra` 是 `forkret`，载入的 `sp` 是进程内核栈顶部。
3. `forkret()` 释放调度器交接过来的 `p->lock`。第一次执行时，它还调用 `fsinit(ROOTDEV)` 初始化文件系统超级块和日志。
4. `forkret()` 调用 `usertrapret()`，后者设置用户陷阱入口、`sepc`、`sstatus` 以及下次进入内核所需的 trapframe 字段。
5. `userret` 切换到该进程的用户页表，恢复用户寄存器。
6. `sret` 切换到 U 模式，并从 `sepc = 0` 开始执行 `initcode`。

这条路径被称为第一次“返回”用户态，但进程以前其实从未在用户态运行；内核通过人工构造 `trapframe`，复用了普通陷阱返回机制。

`initcode` 直接发起：

```asm
la a0, init             # "/init"
la a1, argv
li a7, SYS_exec
ecall
```

`exec("/init", argv)` 从 `fs.img` 读取由 `user/init.c` 构建的 ELF，建立新用户页表和用户栈，再将 `trapframe->epc` 改为新 ELF 的入口。成功后，系统调用返回路径会直接进入新的 `/init` 程序，不会回到旧 `initcode` 的 `ecall` 后面；只有 `exec` 失败时，`initcode` 才会继续执行后面的 `exit` 循环。

## 8. `/init` 为什么通过子进程运行 shell

[init.c](../xv6-labs-2020/user/init.c) 首先打开 `console`。第一次 `open()` 返回最低可用文件描述符 0，作为标准输入；随后两次 `dup(0)` 分别取得文件描述符 1 和 2，作为标准输出和标准错误。

然后 `init` 重复以下过程：

```text
fork()
  ├─ 子进程：exec("sh", argv)
  └─ 父进程 init：wait() 回收退出的子进程
                      ↓
              shell 退出后重新 fork/exec
```

### 红字问题：为什么不让 `init` 自己 `exec` shell

<font color="#c00000">为什么通过子进程运行 shell，而不是让 `init` 进程自己运行？</font>

`init` 必须长期保留为系统的第一个用户进程和兜底父进程：

- 父进程退出时，其尚存子进程会被重新托管给 `init`；`init` 需要不断 `wait()`，防止这些进程退出后永久成为僵尸。
- shell 退出时，保留下来的 `init` 可以再次 `fork/exec`，重新启动一个 shell。
- `init` 与交互式 shell 的生命周期和职责不同；让 PID 1 直接 `exec` 成 shell 会失去专门的孤儿回收者和 shell 监督者。

`init` 的内层 `wait()` 不只等待 shell，也可能先回收到被重新托管的孤儿进程；只有当返回的 PID 等于 shell PID 时，才退出内层循环并重新启动 shell。

## 9. 核心结论

- 当前仓库没有 xv6 自己实现的 `boot` 文件；QEMU 复位 ROM 负责从 `0x1000` 跳到已加载的内核。
- Makefile 组织编译和 QEMU 参数，`kernel.ld` 决定 ELF 地址布局，QEMU 完成实际加载和复位跳转。
- `stack0` 是每 hart 的启动/调度器栈；它不同于每进程内核栈和用户栈。
- `start()` 在 M 模式配置特权级、陷阱委托和机器定时器，再用 `mret` 进入 S 模式的 `main()`。
- 分页在页表创建前保持关闭，由每个 hart 的 `kvminithart()` 分别启用。
- 第一个进程先执行内嵌的 `initcode`，再通过 `exec` 在同一 PID 中变成 `/init`。
- `init` 保留自身并用子进程运行 shell，以便回收孤儿和重启 shell。
