# xv6 系统调用过程

## 1. 一次系统调用的总流程

以下以用户程序调用 `xxx(arg0, arg1, ...)` 为例：

```text
用户程序调用 xxx(...)
        ↓
链接到 usys.o 中的用户态包装函数 xxx
        ↓
参数位于 a0～a5，包装函数把 SYS_xxx 写入 a7
        ↓
执行 ecall，CPU 从用户模式陷入管理模式，并跳到 stvec 指向的 uservec
        ↓
uservec 保存用户寄存器到 trapframe，切换内核页表和内核栈
        ↓
uservec 跳转到 usertrap
        ↓
usertrap 根据 scause 判断这是系统调用，并调用 syscall
        ↓
syscall 从 trapframe->a7 取得系统调用号
        ↓
syscalls[SYS_xxx] 分发到 sys_xxx
        ↓
sys_xxx 从 trapframe->a0～a5 取得参数并完成内核操作
        ↓
syscall 把返回值写入 trapframe->a0
        ↓
usertrapret → userret：切换回用户页表并恢复用户寄存器
        ↓
sret 返回用户模式，从 ecall 后面的 ret 继续执行
        ↓
ret 返回调用 xxx(...) 的用户代码，返回值位于 a0
```

## 2. 用户态系统调用入口的生成与链接

### 2.1 `usys.pl` 是什么

`user/usys.pl` 是一个在**构建阶段**运行的 Perl 代码生成脚本。它根据脚本末尾登记的系统调用名称，批量生成用户态系统调用入口 `user/usys.S`。

例如，脚本中的：

```perl
entry("fork");
```

会生成：

```asm
.global fork
fork:
	 li a7, SYS_fork
	 ecall
	 ret
```

这段代码是 `fork` 的用户态包装函数：

- `.global fork`：导出符号，使链接器能够找到它。
- `li a7, SYS_fork`：把 `fork` 的系统调用号放入寄存器 `a7`。
- `ecall`：触发环境调用，从用户态进入内核态。
- `ret`：内核处理完成后，返回到调用 `fork()` 的用户层代码。 

`usys.pl` 本身不会在 xv6 运行期间执行，也不是用户程序直接调用的文件。

### 2.2 构建阶段发生了什么

Makefile 中的规则为：

```make
$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S
```

构建过程如下：

```text
user/usys.pl
    ↓ Perl 脚本生成
user/usys.S
    ↓ 汇编
user/usys.o
    ↓ 与用户程序及用户库链接
可执行的用户程序
```

执行 `make` 或 `make qemu` 时，如果 `usys.S` 不存在，或 `usys.pl` 比它更新，Make 就会运行该脚本重新生成 `usys.S`。



### 2.3 `fork()` 会被汇编代码替换吗

不会发生 C 源码的文本替换。假设用户程序中写了：

```c
fork();
```

`user/user.h` 提供函数声明：

```c
int fork(void);
```

编译器把 `fork()` 调用编译为对外部符号 `fork` 的函数调用，可以近似理解为：

```asm
call fork
```

链接器随后在 `usys.o` 中找到由 `usys.pl` 生成的 `fork` 符号，并把两者连接起来。因此，更准确的说法是：

> `usys.pl` 为系统调用生成用户态入口；用户程序通过链接找到并调用这些入口，而不是在构建时用汇编代码替换 C 文件中的函数调用。

### 2.4 核心理解

`usys.pl` 的作用可以概括为：

> 在编译 xv6 时，为登记的系统调用生成用户态汇编包装函数；在 xv6 运行时，用户程序调用这些函数，由它们设置系统调用号并执行 `ecall`。

需要特别区分：

- **构建阶段**：运行 `usys.pl`，生成并编译 `usys.S`，再通过链接器连接用户程序。
- **运行阶段**：执行已经生成的包装函数；此时不会再调用 `usys.pl`，也不会修改或替换 C 源码。


## 3. 发起系统调用：用户函数到 `ecall`

用户程序调用 `xxx(...)` 时，RISC-V C 调用约定会把前几个参数放入 `a0`～`a5`。编译器生成的是对外部符号 `xxx` 的普通函数调用，链接器再把该符号解析到 `usys.o` 中的包装函数。

这个链接关系来自 Makefile：

```make
ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
```

`usys.o` 是 `ULIB` 的一部分，因此会和用户程序的目标文件一起传给链接器。包装函数不会改动 `a0`～`a5` 中的参数，只把系统调用号装入 `a7`，然后执行 `ecall`：

```asm
.global xxx
xxx:
 li a7, SYS_xxx
 ecall
 ret
```

## 4. `ecall`：CPU 完成的陷入操作

用户模式执行 `ecall` 后，RISC-V 硬件主要完成以下操作：

1. 把陷入前的特权级记录到 `sstatus.SPP`，把 `sstatus.SIE` 保存到 `sstatus.SPIE`，再清除 `SIE` 以关闭管理模式中断。
2. 把 `ecall` 指令的地址保存到 `sepc`。
3. 把陷阱原因写入 `scause`；来自用户模式的 `ecall` 在 xv6 中对应 `scause == 8`。
4. 将 CPU 特权级切换为管理模式（Supervisor Mode）。
5. 把 `pc` 设置为 `stvec` 中保存的地址，开始执行 `uservec`。

这里需要特别区分 `stvec` 和 `sepc`：

- `stvec` 保存陷阱入口地址，用户态发生陷阱时它指向 `uservec`。
- `sepc` 保存发生陷阱时的用户程序地址，之后供 `sret` 返回使用。

因此，CPU 是把 `pc` 设置为 `stvec`，不是设置为 `sepc`。硬件此时也不会自动切换页表、切换内核栈或保存所有通用寄存器，这些工作由 `uservec` 完成。

## 5. `uservec`：保存现场并进入内核 C 代码

`uservec` 位于 `kernel/trampoline.S`。它开始执行时已经处于管理模式，但仍在使用用户页表。其主要步骤是：

1. 交换 `a0` 和 `sscratch`，取得当前进程映射在 `TRAPFRAME` 地址处的陷阱帧。
2. 把用户态通用寄存器保存到 `trapframe`，包括参数寄存器 `a0`～`a5` 和系统调用号寄存器 `a7`。
3. 从 `trapframe` 中取出内核栈地址、当前 hart ID、`usertrap` 地址和内核页表信息。
4. 切换到内核页表和当前进程的内核栈。
5. 跳转到 `usertrap()`。

蹦床页 `TRAMPOLINE` 在用户页表和内核页表中映射到相同的虚拟地址，因此 `uservec` 在切换页表前后都可以继续执行。

## 6. `usertrap`：识别系统调用

`usertrap()` 位于 `kernel/trap.c`，它首先把 `stvec` 改为 `kernelvec`，使进入内核后再次发生的陷阱由内核陷阱入口处理。随后：

1. 从 `sepc` 读取用户程序计数器，保存到 `p->trapframe->epc`。
2. 检查 `scause`；`scause == 8` 表示来自用户模式的系统调用。
3. 将 `p->trapframe->epc` 加 `4`。因为 `sepc` 指向 `ecall` 本身，返回时应从下一条指令，即包装函数中的 `ret` 开始执行。
4. 调用 `syscall()` 处理系统调用。

注意：`usertrapret()` 设置 `stvec` 指向 `uservec`，同时把用户返回地址写入 `sepc`。它不是把 `sepc` 设置为 `uservec`。

## 7. `syscall`：取得调用号并分发

`uservec` 已经把用户寄存器保存到 `trapframe`，所以 `syscall()` 从 `p->trapframe->a7` 取得系统调用号，而不是依赖此时 CPU 中 `a7` 的现场值：

```c
num = p->trapframe->a7;
p->trapframe->a0 = syscalls[num]();
```

`syscalls` 是以系统调用号为下标的函数指针表。例如：

```c
[SYS_sleep] sys_sleep,
```

因此，`SYS_sleep` 会被分发给 `sys_sleep()`。如果调用号无效，`syscall()` 会打印错误，并把返回值设置为 `-1`。

## 8. 系统调用参数如何取得

包装函数被调用时，参数已经按照 RISC-V C 调用约定放入 `a0`～`a5`；`uservec` 又把这些寄存器保存到了 `trapframe`。内核通过以下函数取出参数：

- `argraw(n)`：从 `trapframe->a0`～`trapframe->a5` 取得第 `n` 个原始参数。
- `argint(n, &value)`：取得整数参数。
- `argaddr(n, &addr)`：取得用户虚拟地址。它只取出地址值，真正访问时还需由 `copyin`、`copyout` 或 `copyinstr` 检查并复制数据。
- `argfd(n, ...)`：取得文件描述符，并检查它是否有效及是否对应已打开文件。

例如，`sys_sleep()` 通过 `argint(0, &n)` 取得 `sleep(n)` 的第一个参数；`sys_read()` 则分别取得文件描述符、用户缓冲区地址和读取长度。

## 9. 返回值和返回用户态

`sys_xxx()` 返回后，`syscall()` 把返回值写入：

```c
p->trapframe->a0 = syscalls[num]();
```

随后按以下顺序返回：

1. `syscall()` 返回 `usertrap()`。
2. `usertrap()` 调用 `usertrapret()`。
3. `usertrapret()` 关闭中断，把 `stvec` 重新设置为 `uservec`，准备下次用户态陷阱所需的 `trapframe` 字段。
4. `usertrapret()` 清除 `sstatus.SPP`，使 `sret` 返回用户模式；同时把保存的用户 `epc` 写入 `sepc`。
5. `usertrapret()` 跳转到蹦床页中的 `userret`。
6. `userret` 切换到用户页表，恢复用户寄存器，其中 `a0` 恢复为系统调用返回值。
7. `sret` 根据 `sepc` 恢复用户态执行。由于 `epc` 已在 `usertrap()` 中加 `4`，此时执行包装函数中 `ecall` 后面的 `ret`。
8. `ret` 返回最初调用 `xxx(...)` 的用户代码；按照 C 调用约定，调用者从 `a0` 得到返回值。

这里更准确的理解不是“`ecall` 结束后执行 `ret`”，而是：`ecall` 触发陷阱，内核处理完成后通过 `sret` 回到 `ecall` 的下一条指令，恰好是包装函数中的 `ret`。

> 以上是普通系统调用的典型返回路径。`exit` 不会返回；`exec` 成功后会转而执行新程序，而不是回到原来的调用点。

## 10. 关键寄存器速查

| 寄存器或字段 | 在系统调用流程中的作用 |
| --- | --- |
| `a0`～`a5` | 保存系统调用参数；返回时 `a0` 保存返回值 |
| `a7` | 保存系统调用号 |
| `stvec` | 保存发生陷阱后要跳转到的入口地址 |
| `sepc` | 保存发生陷阱时的用户 `pc`，供 `sret` 返回 |
| `scause` | 保存陷阱原因，用户态 `ecall` 对应值 `8` |
| `sscratch` | 帮助 `uservec` 找到并操作当前进程的 `trapframe` |
| `satp` | 保存当前页表信息，用于切换用户页表和内核页表 |
| `trapframe->epc` | xv6 保存的用户返回地址，最终写回 `sepc` |

## 11. 参考位置

- 用户态包装函数生成：`user/usys.pl`
- 用户程序链接规则：`Makefile` 中的 `ULIB` 和 `_%: %.o $(ULIB)`
- 用户态陷阱入口及返回：`kernel/trampoline.S` 中的 `uservec`、`userret`
- 用户陷阱处理：`kernel/trap.c` 中的 `usertrap`、`usertrapret`
- 系统调用参数与分发：`kernel/syscall.c`
- 进程类系统调用实现：`kernel/sysproc.c`
- 文件类系统调用实现：`kernel/sysfile.c`
- 教材：[4.2 从用户空间陷入](https://xv6.dgs.zone/tranlate_books/book-riscv-rev1/c4/s2.html)、[4.3 代码：调用系统调用](https://xv6.dgs.zone/tranlate_books/book-riscv-rev1/c4/s3.html)、[4.4 系统调用参数](https://xv6.dgs.zone/tranlate_books/book-riscv-rev1/c4/s4.html)
