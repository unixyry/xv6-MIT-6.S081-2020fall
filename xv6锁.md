# xv6 的锁

xv6 提供两种内核锁：自旋锁（`spinlock`）在拿不到锁时反复尝试，适合很短的临界区；睡眠锁（`sleeplock`）在等待时让出 CPU，适合可能持续较久的操作。两者都用于互斥，但等待方式和持锁期间能否让出 CPU 这两个行为有不同之处。

## 自旋锁：短临界区内的互斥

自旋锁的主要接口是 `acquire` 和 `release`。`lk->locked` 为 `0` 表示锁空闲，为 `1` 表示已被占用。下面的代码摘录自 [spinlock.c](../xv6-labs-2020/kernel/spinlock.c) 的核心路径，省略了 `LAB_LOCK` 条件编译下的锁竞争统计代码。

### 获取自旋锁

`acquire` 先调用 `push_off()` 关闭当前 CPU 的中断，以避免同一 CPU 的中断处理程序再次申请这把锁而死锁；`holding(lk)` 则防止当前 CPU 重复获取已持有的锁。随后，`__sync_lock_test_and_set(&lk->locked, 1)` 原子地把 `1` 写入 `lk->locked`，并返回写入前的值。它相当于交换寄存器中的 `1` 与这一个内存位置的旧值，而不是交换两个内存变量。返回 `0` 的 CPU 成功获取锁；返回非零时，循环继续尝试。获取成功后，`lk->cpu` 记录持锁 CPU，供 `holding()` 和调试使用。

```c
void
acquire(struct spinlock *lk)
{
  push_off();
  if(holding(lk))
    panic("acquire");

  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  __sync_synchronize();
  lk->cpu = mycpu();
}
```

在当前源码的注释中，RISC-V 上取锁的原子交换指令写作 `amoswap.w.aq`。原子交换保证两个 CPU 不会同时把同一个 `0` 改成 `1`；循环负责在锁已占用时重试。`push_off()` 只影响当前 CPU，并不会阻止其他 CPU 并发执行。

### 释放自旋锁

`release` 先检查当前 CPU 是否持有锁，再清除调试字段 `lk->cpu`。它在解锁前调用 `__sync_synchronize()`，随后通过 `__sync_lock_release(&lk->locked)` 将锁状态原子地置为 `0`。源码不用普通 C 赋值解锁，是为了明确依赖内建操作的原子更新和释放语义，而不依赖编译器如何实现赋值；其 RISC-V 注释将释放操作写作一次写入 `0` 的 `amoswap.w`。最后，`pop_off()` 与 `push_off()` 配对：当嵌套的自旋锁全部释放后，恢复进入最外层临界区前的中断状态。

```c
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;
  __sync_synchronize();
  __sync_lock_release(&lk->locked);

  pop_off();
}
```

### 红字问题一：`__sync_synchronize` 的作用

<font color="#c00000">`__sync_synchronize` 啥作用，为什么需要？怎么使用的？</font>

`__sync_synchronize()` 是**完整内存屏障**：它约束编译器和 CPU，不让屏障前后的内存读写跨越屏障重排。在 `acquire` 中，它放在成功获取锁之后，要求临界区的读写发生在取锁之后；在 `release` 中，它放在清除锁状态之前，要求临界区的读写先完成，再让其他 CPU 看见锁已释放。调用时没有必需参数，也不针对某一个特定变量。它提供的是**内存操作的顺序约束**，锁状态更新的原子性则由原子交换操作提供。

例如，持锁 CPU 先更新共享数据，再释放锁。若没有合适的排序约束，另一个 CPU 可能先观察到锁已空闲，却看不到刚才的完整更新。按照 [GCC 对这些内建操作的定义](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fsync-Builtins.html)，`__sync_lock_test_and_set` 本身已有 acquire（获取）语义，`__sync_lock_release` 本身已有 release（释放）语义；xv6 额外使用两次完整屏障，提供更强、更保守的排序保证。因此，需要的是取锁与解锁之间的正确内存顺序，不能把这两次调用理解为原子交换的唯一来源。

### 红字问题二：`__sync_*` 为何没有源码定义

<font color="#c00000">为什么在代码中看不到 `__sync_lock_test_and_set`、`__sync_synchronize` 和 `__sync_lock_release` 的定义？它们的实现在哪里，链接的时候又是如何找到函数实现的？为什么用这个截然不同的命名方式？</font>

这三个名字是 **GCC 内建函数**，不需要在 xv6 源码中另写 C 函数定义。编译器在编译时识别调用，并由目标架构的代码生成部分将其展开成原子指令和内存屏障指令。在这里的 RISC-V 路径上，`__sync_lock_test_and_set` 对应原子交换，`__sync_synchronize` 对应内存屏障，`__sync_lock_release` 对应原子置零操作。它们直接变成目标代码后，链接器通常**不需要寻找同名函数符号**。

这种直接展开取决于目标架构是否支持该操作。GCC 文档说明：若某种操作无法在目标上直接实现，编译器可能生成对外部辅助函数的调用，并给名称加上数据宽度后缀；那时链接器才需要由目标运行时或库提供相应实现。当前 xv6 的 RISC-V 自旋锁路径使用原子指令，不走这条后备路径。

`__sync_*` 的命名沿用了 Intel Itanium 处理器 ABI 中的接口，以保持兼容，因此没有采用 GCC 通常使用的 `__builtin_*` 前缀。这组接口属于较早的原子操作内建函数；[GCC 文档](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fsync-Builtins.html) 建议新代码使用能明确指定内存顺序的 `__atomic_*` 接口。

## 睡眠锁：等待时让出 CPU

当临界区可能等待磁盘 I/O 等较慢操作时，持有自旋锁会让其他申请者持续占用 CPU。睡眠锁的主要接口是 `acquiresleep` 和 `releasesleep`。睡眠锁内部的自旋锁 `lk->lk` **只保护** `lk->locked` 和 `lk->pid` 等锁状态；进程取得睡眠锁后便释放 `lk->lk`，不会在整个较长的操作期间持有自旋锁。以下代码来自 [sleeplock.c](../xv6-labs-2020/kernel/sleeplock.c)。

### 获取睡眠锁

`acquiresleep` 先取得内部自旋锁，检查 `lk->locked`。若睡眠锁已被占用，就调用 `sleep(lk, &lk->lk)`：`lk` 是睡眠通道，`&lk->lk` 是检查锁状态时持有的自旋锁。根据 [proc.c 中的 `sleep` 实现](../xv6-labs-2020/kernel/proc.c)，进程在进入睡眠时释放 `lk->lk`，让其他进程能够释放睡眠锁；被唤醒后又重新取得 `lk->lk`，再返回 `acquiresleep`。`sleep` 在释放原锁之前取得进程锁 `p->lock`，并在该锁保护下设置睡眠状态，因此不会错过并发的 `wakeup`。

```c
void
acquiresleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}
```

这里必须使用 `while` 而不是只检查一次：被唤醒仅表示可以再次竞争睡眠锁，不保证当前进程已经得到它。只有重新检查到 `lk->locked == 0` 的进程，才在内部自旋锁保护下把它置为 `1`，并记录持锁进程的 PID。

### 释放睡眠锁

`releasesleep` 取得内部自旋锁后，将 `lk->locked` 和 `lk->pid` 清零；`wakeup(lk)` 把在通道 `lk` 上睡眠的进程设为可运行，然后释放内部自旋锁。这些进程会在之后由调度器安排运行，并各自重新检查锁状态；即使唤醒了多个进程，也只有一个能取得睡眠锁。

```c
void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}
```

睡眠锁适用于可能持续较久、持锁期间需要让出 CPU 的操作。`acquiresleep` 可能睡眠，因此不能在中断处理程序中调用，也不能在已持有自旋锁的临界区内调用。
