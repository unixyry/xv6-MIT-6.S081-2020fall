# xv6 进程调度

xv6 的调度核心是：每个 CPU 都运行一个 `scheduler()` 循环；进程需要让出 CPU 时，先修改自己的状态，再通过 `sched()` 和 `swtch()` 切换到该 CPU 的调度器上下文；调度器随后寻找 `RUNNABLE` 进程并切换过去。

调度不只发生在“主动睡眠”和“时间片到期”两种情况下。当前运行进程进入调度器的主要入口包括：

- 时钟中断触发 `yield()`：进程由 `RUNNING` 变为 `RUNNABLE`，之后仍可再次运行。
- 等待管道、磁盘、子进程或时间等事件而调用 `sleep()`：进程变为 `SLEEPING`。
- 调用 `exit()`：进程变为 `ZOMBIE`，等待父进程通过 `wait()` 回收。
- 内核代码显式调用 `yield()`：主动放弃本轮 CPU。

这里的“用户态进程”和“内核态进程”并不是两类进程。同一个进程可以在用户态执行，也可以因系统调用、中断或异常进入内核态；只有进入内核态后，它才能调用调度代码。

## 1. 进程状态与状态转换

[proc.h](../xv6-labs-2020/kernel/proc.h) 定义了五种进程状态：

| 状态 | 含义 |
| --- | --- |
| `UNUSED` | 进程表项空闲，可以被 `allocproc()` 使用 |
| `SLEEPING` | 正在等待 `chan` 所代表的事件，不参与调度 |
| `RUNNABLE` | 已具备运行条件，等待某个 CPU 的调度器选中 |
| `RUNNING` | 当前正在某个 CPU 上运行 |
| `ZOMBIE` | 已退出但尚未被父进程回收，仍保留 PID 和退出状态等信息 |

主要状态转换如下：

```text
UNUSED ──创建──> RUNNABLE ──scheduler──> RUNNING
                         <── yield ────────┘

RUNNING ──sleep──> SLEEPING ──wakeup/kill──> RUNNABLE
RUNNING ──exit───> ZOMBIE   ──wait/freeproc──> UNUSED
```

`wakeup()` 只把进程从 `SLEEPING` 改为 `RUNNABLE`，并不保证它立即运行。它还要等待某个 CPU 的 `scheduler()` 选中。

## 2. 两种“上下文”必须区分

xv6 中最容易混淆的是 `trapframe` 和 `context`。二者都保存寄存器，但所跨越的边界、保存内容和恢复方式不同。

| 对比项 | `struct trapframe` | `struct context` |
| --- | --- | --- |
| 跨越的边界 | 用户态与内核态 | 进程的内核执行流与每 CPU 调度器 |
| 保存位置 | `p->trapframe` | `p->context` 或 `cpu->context` |
| 主要代码 | `uservec` / `userret` | `swtch` |
| 保存内容 | `uservec` 保存用户通用寄存器，`usertrap()` 将用户返回地址记入 `epc`；还包括再次进内核所需信息 | `ra`、`sp` 和 `s0`～`s11` |
| 是否切页表 | `uservec`/`userret` 会切换内核/用户页表 | 不切换页表 |
| 是否切特权级 | 最终由 `sret` 返回用户态 | 始终在 S 模式内核态运行 |

来自用户态的陷阱发生时，`trampoline.S` 中的 `uservec` 必须保存几乎完整的用户寄存器，因为用户代码并没有按普通 C 函数调用约定主动调用内核。`swtch()` 则像一次特殊的内核函数调用，只需保存 RISC-V 调用约定要求被调用者保持的寄存器；调用者保存寄存器已经由编译器在需要时放入当前内核栈。

若陷阱发生在内核态，入口是 `kernelvec`，它把完整寄存器现场压入当前内核栈（进程运行时即该进程的内核栈）；这又不同于 `trapframe` 和 `context`。

### 红字问题：为什么 `swtch` 和 `trampoline` 不能互换

<font color="#c00000">为什么 `swtch` 和 `trampoline` 不能相互替换？同样都是上下文，两者的使用场景有什么不同？</font>

首先，`trampoline` 是包含 `uservec` 和 `userret` 的一页代码，不是一个上下文结构。它负责用户态/内核态边界协议中的汇编部分：保存或恢复完整用户寄存器、切换页表与栈，并通过 `sret` 改变特权级；`trap.c` 则配合设置 `stvec`、`sepc` 和 `sstatus`。这条路径使用的是 `p->trapframe`。

`swtch` 只在内核态切换两条内核执行流。它把旧执行流的 `ra`、`sp` 和 `s0`～`s11` 写入一个 `struct context`，再载入另一个 `struct context`，最后由 `ret` 跳到新上下文的 `ra`。它既不处理 `sepc`、`sstatus`、`stvec`，也不切页表或特权级。

因此二者不能替换：用 `swtch` 进入或离开用户态会丢失未保存的用户寄存器，也没有完成页表和特权级切换；用 `trampoline` 做内核调度则依赖陷阱 CSR 和 `trapframe`，还会错误地走用户态返回协议。

## 3. 时钟抢占下的一次完整调度

![[图6 - xv6进程调度流程.png]]

<center>图 6：从进程 A 切换到进程 B 的调度流程</center>

图中的流程应分成三段理解：保存 A 的用户现场、在内核中切换调度上下文、恢复 B 的用户现场。假设时钟中断发生在用户态进程 A，且调度器随后选中进程 B：

1. 每个 hart 的机器模式定时器产生中断。当前仓库的 `timervec` 安排下一次定时器中断，并设置 `sip.SSIP`，把它转成 S 模式软件中断；`devintr()` 最终以返回值 `2` 表示时钟中断。
2. CPU 从用户态进入 S 模式并跳到 `uservec`。`uservec` 把 A 的用户寄存器保存到 `A->trapframe`，切换到全局内核页表和 A 的内核栈，再跳转到 `usertrap()`。
3. `usertrap()` 识别时钟中断并调用 `yield()`。`yield()` 获取 `A->lock`，把 A 从 `RUNNING` 改为 `RUNNABLE`，然后调用 `sched()`。
4. `sched()` 用 `swtch(&A->context, &cpu->context)` 保存 A 当前的内核调用链，并恢复该 CPU 上一次保存的调度器上下文。
5. `scheduler()` 从之前的 `swtch()` 返回，清除 `cpu->proc` 并释放 `A->lock`；随后继续扫描进程表。
6. 找到 B 后，调度器获取 `B->lock`，把 B 改为 `RUNNING`，设置 `cpu->proc = B`，再执行 `swtch(&cpu->context, &B->context)`。
7. 若 B 以前让出过 CPU，它会从自己先前的 `sched()` 返回，并继续执行当时尚未完成的 `yield()`、`sleep()` 或内核陷阱路径；若 B 是第一次被调度的新进程，则从预设的 `forkret()` 开始。
8. 只有当 B 的内核控制流走到 `usertrapret()` 和 `userret` 时，才会切换到 B 的用户页表、恢复 `B->trapframe` 中的用户寄存器，并通过 `sret` 从 `sepc` 指定的地址返回用户态。

第 7、8 步修正了一个常见误解：调度器切到 B 后不一定立刻执行 `usertrapret()`。B 恢复的是它上次停止的内核执行点。例如，若 B 在内核态时被时钟中断抢占，它恢复后会先回到 `kerneltrap()`，再由 `kernelvec` 返回原来的内核代码。

`yield()` 也不保证一定换成另一个进程；如果没有更合适的可运行进程，A 之后仍可能再次被选中。

### 3.1 `pc`、`sepc` 与 `ra`：两种不同的返回

| 名称 | 含义 | 在这里的作用 |
| --- | --- | --- |
| `pc`（程序计数器） | CPU 当前执行位置，不是可像 `ra` 那样直接读写的通用寄存器 | 跳转、陷阱返回或函数返回都会改变下一条指令的位置 |
| `sepc`（S 模式异常程序计数器） | 记录陷阱指令或中断后的续执行地址的 CSR；xv6 还将其暂存于 `p->trapframe->epc` | `sret` 根据 `sepc` 设置返回后的 `pc` |
| `ra`（`x1`，返回地址寄存器） | 一次普通函数调用的返回地址 | `ret` 根据 `ra` 设置下一条指令的 `pc`；`sret` 不使用 `ra` |

执行用户态的 `call xxx` 时，`call` 是伪指令，由汇编器展开、可能经链接器松弛为带链接的跳转指令：它把**调用指令序列后面那条指令的地址**写入 `ra`，并跳到 `xxx`。若实际使用一条 4 字节的 `jal`，该地址就是这条 `jal` 的 `pc + 4`；`call` 也可能展开为多条指令，所以不宜笼统说 `ra` 总是 `pc + 4`。`ret` 通常展开为 `jalr x0, 0(ra)`，把控制流交还给这个调用后的地址。`ra` 不是“当前 `pc`”或“陷阱返回地址”。此外，`p->trapframe->ra` 保存的是用户态 `ra`，而 `swtch()` 保存到 `p->context.ra` 的是内核执行流的返回地址，两者也不能混用。

以一个**会正常返回**的系统调用为例，用户程序调用 `xxx()`，进入由 `usys.pl` 在构建时生成的 `usys.S` 包装函数：

```text
调用者：call xxx         （ra ← 调用者中 call 后的地址 L）
        L: ...           （函数返回后，从这里继续）

包装函数 xxx：
        li a7, SYS_xxx
        ecall            （陷阱发生时 sepc 指向这里）
        ret              （sret 后先执行这里，再按 ra 跳到 L）
```

`ecall` 是同步异常，硬件把它自身的地址写入 `sepc`，并不修改用户的 `ra`。`uservec` 将用户 `ra` 存入 `trapframe`；`usertrap()` 再把 `sepc` 存入 `p->trapframe->epc`，且**只在系统调用分支**执行 `epc += 4`，从而跳过 4 字节的 `ecall`。返回前，`usertrapret()` 将 `epc` 写回 `sepc`，`userret` 切回用户页表并恢复包括 `ra` 在内的用户寄存器；最后的 `sret` 才依据 `sepc` 改变 `pc`，并按 `sstatus.SPP` 返回用户模式。此时 `pc` 指向包装函数中的 `ret`，执行 `ret` 后 `pc` 才跳到调用者中 `call xxx` 之后。这里是先 `sret`、后 `ret` 的两级返回，不能把两者的返回地址混为一谈。

如果是**来自用户态的中断**，`usertrap()` 保存 `sepc` 到 `epc`，但不执行 `epc += 4`。处理完成后，`sret` 返回中断发生时应继续执行的用户指令地址，而不是像系统调用那样跳过一条指令；此时也不一定会立即执行 `ret`。如果中断发生在**内核态**，则走 `kernelvec → kerneltrap() → kernelvec` 的返回路径，由该路径的 `sret` 回到 S 模式内核代码，而不是经 `userret` 直接回用户态。

上述“系统调用返回到包装函数的 `ret`”有例外：成功的 `exec()` 会把 `p->trapframe->epc` 改为新程序入口，`exit()` 则不返回。这两种情况都不会再执行原包装函数中的 `ret`。

## 4. `scheduler`、`sched`、`yield` 和 `swtch`

### 4.1 `scheduler()`：每 CPU 的调度循环

每个 CPU 都有自己的 `struct cpu` 和 `cpu->context`，并独立运行 `scheduler()`。调度器不是进程表中的普通进程；它是一段每 CPU 执行流。当前实现顺序扫描进程表，选中一个 `RUNNABLE` 进程：

```c
acquire(&p->lock);
if(p->state == RUNNABLE) {
  p->state = RUNNING;
  c->proc = p;
  swtch(&c->context, &p->context);
  c->proc = 0;
}
release(&p->lock);
```

在这段代码中，`swtch()` 返回并不表示刚刚选中的进程“启动完成”，而表示该进程后来又通过 `sched()` 切回了这个调度器执行点。

### 4.2 `sched()`：带不变量检查的切换入口

`sched()` 不选择下一个进程，只负责从当前进程切回本 CPU 的调度器。调用它之前必须满足：

- 持有且只持有当前进程的 `p->lock`；
- `p->state` 已经不再是 `RUNNING`；
- 当前 CPU 的中断已经关闭。

其核心语句是：

```c
swtch(&p->context, &mycpu()->context);
```

`sched()` 还保存并恢复 `cpu->intena`。这是因为“进入最外层 `push_off()` 之前中断是否开启”属于被切换的内核执行流；进程再次运行时可能已经迁移到另一个 CPU，不能简单沿用该 CPU 上另一个执行流留下的值。

### 4.3 `yield()`：把自己重新放回可运行队列

```c
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}
```

从源码表面看，`yield()` 在 `sched()` 后才释放 `p->lock`。实际执行过程跨越了两次调度：当前进程第一次执行 `sched()` 时切回调度器，由调度器释放这把锁；将来调度器再次选中该进程时，又会先获取同一把锁，然后切回 `sched()`，最后由恢复运行的进程执行 `release(&p->lock)`。这是有意设计的“锁交接”。

新进程没有旧的 `sched()` 调用点，因此 `allocproc()` 把 `p->context.ra` 初始化为 `forkret`；第一次 `swtch()` 到它时，`forkret()` 先释放调度器交接过来的 `p->lock`，再调用 `usertrapret()`。

### 4.4 为什么切换时要持有 `p->lock` 并关闭中断

`p->lock` 保护的不只是 `p->state` 这个字段，还保护“进程状态、内核上下文和 CPU 归属彼此一致”这一组调度不变量。进程在 `swtch()` 完成前仍可能使用其内核栈和上下文；若过早暴露 `RUNNABLE` 且释放锁，另一个 CPU 可能同时选中它，造成同一进程在两个 CPU 上运行。

关闭中断也不能简单解释成“`swtch` 的每条指令都绝对不可中断，否则寄存器无法恢复”。`acquire()` 会通过 `push_off()` 关闭本 CPU 中断，`sched()` 再检查该条件。这样可以避免在 `p->state`、`cpu->proc` 和上下文处于过渡状态时，在同一 CPU 上重入陷阱或调度代码，也保证 `mycpu()` 相关状态在临界区内稳定。`sched()` 要求 `cpu->noff == 1`，防止进程带着其他自旋锁睡眠或让出 CPU。

### 4.5 `swtch()` 为什么会在另一处“返回”

`swtch()` 保存旧上下文的 `ra` 和 `sp`，载入新上下文的对应值，然后执行 `ret`。RISC-V 的 `ret` 使用刚载入的 `ra` 作为下一条指令地址，所以第一次切换会在新执行流保存的调用点之后继续，而不是回到旧执行流。只有以后再次恢复旧上下文时，旧的 `swtch()` 调用才会表现为返回。

内核调度期间 `satp` 保持为全局内核页表；`swtch()` 本身不负责在 A、B 的用户页表之间切换。B 最终执行 `userret` 时才载入 B 的用户页表。

## 5. `sleep()` 与 `wakeup()`：等待事件

`sleep()` 和 `wakeup()` 不是一对信号量操作。`chan` 只是内核用作“等待事件标识”的任意地址：xv6 不解释它指向的值，也不会为它保存信号计数。若 `wakeup(chan)` 发生时没有进程睡在该通道上，这次唤醒不会留到以后使用。

### 5.1 `sleep(chan, lk)` 做了什么

调用者在检查“条件尚未满足”时持有条件锁 `lk`，然后调用 `sleep(chan, lk)`：

1. 若 `lk` 不是 `p->lock`，先获取 `p->lock`，再释放 `lk`。
2. 设置 `p->chan = chan`、`p->state = SLEEPING`。
3. 调用 `sched()` 切回调度器。
4. 被唤醒并再次获得 CPU 后，清除 `p->chan`。
5. 若 `lk != &p->lock`，释放 `p->lock` 并重新获取调用者传入的 `lk`；若两者本来就是同一把锁，则保持持锁状态返回。

“获取 `p->lock` 后再释放条件锁”使检查条件与进入睡眠对 `wakeup()` 来说是原子的，从而避免丢失唤醒。若先释放 `lk`，再设置 `SLEEPING`，另一个 CPU 可能在两者之间改变条件并执行 `wakeup()`；由于此时进程尚未睡眠，那次唤醒会丢失，进程随后可能永久睡下去。

### 5.2 `wakeup(chan)` 做了什么

`wakeup(chan)` 扫描进程表，逐个获取 `p->lock`，把满足下面条件的进程改为 `RUNNABLE`：

```c
p->state == SLEEPING && p->chan == chan
```

它会唤醒该通道上的所有睡眠进程。被唤醒只表示可以重新竞争 CPU 和相关资源；条件可能已被其他进程抢先改变，因此调用者通常需要在 `while` 循环中重新检查等待条件。

## 6. `exit()` 与 `wait()`：退出和回收

### 6.1 `exit()` 的执行过程

内核中的 `exit(status)` 不会返回。它主要完成以下工作：

1. 关闭当前进程的所有打开文件。
2. 释放当前工作目录 `p->cwd` 的 inode 引用。
3. 把自己的子进程重新托管给 `init`，使孤儿进程最终仍能被回收。
4. 唤醒可能正在 `wait()` 的父进程。
5. 保存退出状态到 `p->xstate`，把状态改为 `ZOMBIE`。
6. 调用 `sched()` 永久让出 CPU。

`exit()` 此时不能直接释放自己的内核栈和整个进程表项，因为当前代码还运行在自己的内核栈上；而且父进程还需要读取 PID 和退出状态。最终回收由父进程的 `wait()` 完成。

### 红字问题：`exit` 和 `return` 的区别

<font color="#c00000">`exit` 和 `return` 有什么区别？</font>

`return` 结束当前函数，把控制流交还给调用者，进程仍然存在；`exit(status)` 结束整个进程，关闭资源、记录退出状态、进入 `ZOMBIE`，并且永不返回调用点。

在这个 xv6 版本中，用户程序的链接入口就是 `main`，没有完整 C 运行库自动把 `main` 的返回值转换成 `exit()`，所以用户程序应显式调用 `exit(status)`。用户态 `exit()` 包装函数通过系统调用进入内核，最终调用这里的内核 `exit()`。

### 红字问题：`p->cwd` 是什么

<font color="#c00000">`p->cwd` 是什么？</font>

`p->cwd` 是指向当前工作目录 inode 的指针（current working directory），不是路径字符串。解析相对路径时，[fs.c](../xv6-labs-2020/kernel/fs.c) 中的 `namex()` 从 `myproc()->cwd` 开始；绝对路径则从根目录开始。

- `fork()` 通过 `idup(p->cwd)` 增加 inode 引用计数，使父子进程各自持有引用。
- `chdir()` 验证新目标是目录，释放旧引用并更新 `p->cwd`。
- `exit()` 通过 `iput(p->cwd)` 释放当前进程持有的引用。`iput()` 可能更新文件系统元数据，所以这里被 `begin_op()` / `end_op()` 包在日志事务中。

### 6.2 `wait()` 如何回收子进程

`wait(addr)` 持有父进程自己的锁并反复扫描进程表：

- 找到一个 `ZOMBIE` 子进程时，把其 `xstate` 复制到用户地址 `addr`（若 `addr != 0`），调用 `freeproc()` 释放 trapframe、用户页表和进程表项，并返回该子进程 PID。
- 没有子进程，或者父进程已被杀死时，返回 `-1`。
- 有子进程但尚无 `ZOMBIE` 时，执行 `sleep(p, &p->lock)`，睡在以父进程指针 `p` 为通道的等待队列上。

子进程 `exit()` 时调用 `wakeup1(original_parent)` 唤醒父进程。父进程醒来后必须重新扫描，因为一次唤醒不对应某个预先绑定的子进程。一次 `wait()` 最多回收一个子进程；回收多个子进程必须调用多次 `wait()`。

## 7. 管道如何使用 `sleep()` / `wakeup()`

[pipe.c](../xv6-labs-2020/kernel/pipe.c) 中的管道由一个 `struct pipe` 表示，包含：

- `data[PIPESIZE]`：内核环形缓冲区；
- 单调递增的 `nread` 和 `nwrite`：已读、已写字节总数，数组下标使用 `% PIPESIZE`；
- `readopen` 和 `writeopen`：读端、写端是否仍打开；
- `lock`：保护以上共享状态并配合睡眠，避免丢失唤醒。

### 7.1 创建与关闭

`pipealloc()` 分配两个 `struct file` 和一个 `struct pipe`。第一个文件只读，第二个文件只写，它们共同指向同一个管道对象。

`pipeclose()` 关闭一端时必须唤醒另一端：关闭写端会唤醒睡在 `&pi->nread` 上的读者，使其在缓冲区耗尽后读到 EOF；关闭读端会唤醒睡在 `&pi->nwrite` 上的写者，使其发现 `readopen == 0` 并返回错误。两端都关闭后才能释放管道内存。

### 7.2 写端 `pipewrite()`

写端持有 `pi->lock` 并循环写入：

```text
读端已关闭或当前进程被杀死：返回 -1
缓冲区已满（nwrite == nread + PIPESIZE）：
    唤醒读者
    sleep(&pi->nwrite, &pi->lock)，以 &nwrite 为通道等待出现空间
缓冲区有空间：
    copyin 一个字节
    data[nwrite % PIPESIZE] = ch
    nwrite++
写入结束：唤醒读者并返回已写字节数
```

写者睡在 `&pi->nwrite` 上；读者每次取走数据后递增 `nread`，并通过 `wakeup(&pi->nwrite)` 通知写者“可能已有空间”。

### 7.3 读端 `piperead()`

读端也持有 `pi->lock`：

```text
缓冲区为空且写端仍打开：
    若当前进程被杀死，返回 -1
    sleep(&pi->nread, &pi->lock)，以 &nread 为通道等待出现数据
缓冲区有数据：
    最多读取 n 个字节
    ch = data[nread % PIPESIZE]
    nread++
    copyout 到用户缓冲区
读取结束：唤醒写者并返回已读字节数
```

若缓冲区为空且写端已经关闭，`while` 条件不成立，`piperead()` 返回 `0`，这就是管道 EOF。读者睡在 `&pi->nread` 上；写者写入后递增 `nwrite`，并通过 `wakeup(&pi->nread)` 通知读者“可能已有数据”。

这里使用“可能”是因为 `wakeup()` 会唤醒同一通道上的所有进程。某个进程真正获得 CPU 前，数据或空间可能已经被另一个进程消耗，因此代码必须重新检查条件。`pi->lock` 与 `sleep(chan, &pi->lock)` 的原子锁交接保证了检查“满/空”和进入睡眠之间不会丢失唤醒。

## 8. 核心结论

- `trapframe` 保存用户态现场，`context` 保存内核调度现场；二者不能替代。
- `swtch()` 只切换内核栈和少量被调用者保存寄存器，不切页表、不改变特权级。
- `p->lock` 在进程与调度器之间跨 `swtch()` 交接，用来维持状态、CPU 归属和上下文的一致性。
- `sleep()` / `wakeup()` 是基于等待通道的睡眠与唤醒机制，不是自带计数的信号量。
- `exit()` 只把进程推进到 `ZOMBIE`；`wait()` 才完成最终回收。
- 管道用同一把锁保护缓冲区条件，并用两个不同通道分别等待“有数据”和“有空间”。
