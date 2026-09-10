

本文前两节集中提供`risc-v`寄存器和指令总表，后续章节说明反汇编、函数栈帧、系统调用存根和 `trampoline.S` 的具体用法。

## 1. 寄存器总表

### 1.1 整数寄存器

`x10` 与 `a0` 是同一个寄存器的两种名称。汇编代码通常使用更容易理解的 ABI 名称。

| 原始编号 | ABI 名称 | 调用约定 | 常见用途及 xv6 中的用法 |
| ---: | --- | --- | --- |
| `x0` | `zero` | 固定值 | 零寄存器：永远读取为 `0`，写入会被忽略；用于构造常量或丢弃结果 |
| `x1` | `ra` | 调用者保存 | 返回地址；`call` 写入返回地址，`ret` 根据它返回 |
| `x2` | `sp` | 被调用者保存 | 栈指针；分配栈帧时减小，释放栈帧时增大 |
| `x3` | `gp` | 固定用途 | 全局指针；普通函数不应随意修改 |
| `x4` | `tp` | 固定用途 | 线程指针；xv6 内核中用于保存当前 hart ID |
| `x5`～`x7` | `t0`～`t2` | 调用者保存 | 临时寄存器；调用其他函数后可能被覆盖 |
| `x8` | `s0` / `fp` | 被调用者保存 | 保存寄存器；也常作为帧指针 `fp` 使用 |
| `x9` | `s1` | 被调用者保存 | 保存需要跨函数调用继续使用的数据 |
| `x10` | `a0` | 调用者保存 | 第 1 个参数和第 1 个返回值；trampoline 中也临时保存 `TRAPFRAME` 地址 |
| `x11` | `a1` | 调用者保存 | 第 2 个参数和第 2 个返回值；调用 `userret` 时保存用户页表信息 |
| `x12` | `a2` | 调用者保存 | 第 3 个参数 |
| `x13` | `a3` | 调用者保存 | 第 4 个参数 |
| `x14` | `a4` | 调用者保存 | 第 5 个参数 |
| `x15` | `a5` | 调用者保存 | 第 6 个参数 |
| `x16` | `a6` | 调用者保存 | 第 7 个参数 |
| `x17` | `a7` | 调用者保存 | 第 8 个参数；xv6 用户态系统调用号 |
| `x18`～`x27` | `s2`～`s11` | 被调用者保存 | 保存需要跨函数调用继续使用的数据 |
| `x28`～`x31` | `t3`～`t6` | 调用者保存 | 临时寄存器；调用其他函数后可能被覆盖 |
| — | `pc` | 非通用寄存器 | 程序计数器；表示当前执行位置，不能像 `x0`～`x31` 那样直接作为普通操作数 |

调用者保存表示调用函数前由调用方按需保存；被调用者保存表示被调用函数若要修改，必须先保存并在返回前恢复。

函数参数依次使用 `a0`～`a7`。例如：

| 调用 | `a0` | `a1` | `a2` |
| --- | --- | --- | --- |
| `printf("%d %d\n", 12, 13)` | 格式字符串地址 | `12` | `13` |

### 1.2 xv6 系统调用路径常见 CSR

CSR（Control and Status Register）不是整数通用寄存器，只能通过 CSR 指令访问。

| CSR | 作用 | 在 xv6 系统调用路径中的意义 |
| --- | --- | --- |
| `sscratch` | Supervisor 模式临时寄存器 | 在 `a0` 与 `TRAPFRAME` 地址之间交换、暂存数据 |
| `satp` | 地址转换与保护 | 保存当前页表信息，用于切换用户页表和内核页表 |
| `stvec` | Supervisor 陷阱向量基址 | 保存发生陷阱后跳转的入口地址，例如 `uservec` |
| `sepc` | Supervisor 异常程序计数器 | 保存陷阱发生处的 `pc`，`sret` 根据它返回 |
| `scause` | Supervisor 陷阱原因 | `usertrap` 用它判断是否发生了系统调用等陷阱 |
| `sstatus` | Supervisor 状态 | 保存返回特权级和中断状态，供 `sret` 使用 |

## 2. 指令总表

### 2.1 真实指令、压缩指令和伪指令

| 指令 | 类型 | 基本格式 | 作用与常见用途 |
| --- | --- | --- | --- |
| `addi` | 真实指令 | `addi rd, rs1, imm` | 加立即数：`rd = rs1 + imm`；如 `addi sp, sp, -16` 分配栈空间 |
| `addiw` | 真实指令 | `addiw rd, rs1, imm` | 低 32 位加立即数，再符号扩展到 64 位；常用于 C 的 `int` 运算 |
| `addw` | 真实指令 | `addw rd, rs1, rs2` | 两个寄存器的低 32 位相加，再符号扩展到 64 位 |
| `slliw` | 真实指令 | `slliw rd, rs1, shamt` | 将低 32 位左移，再符号扩展到 64 位；左移 1 位常用于乘 2 |
| `auipc` | 真实指令 | `auipc rd, imm20` | `rd = 当前指令地址 + (imm20 << 12)`；用于 PC 相对寻址 |
| `lw` | 真实指令 | `lw rd, offset(rs1)` | 从 `rs1 + offset` 读取 4 字节，并在 RV64 中符号扩展到 64 位 |
| `sw` | 真实指令 | `sw rs2, offset(rs1)` | 把 `rs2` 的低 4 字节写入 `rs1 + offset` |
| `ld` | 真实指令 | `ld rd, offset(rs1)` | 从 `rs1 + offset` 读取 8 字节到 `rd` |
| `sd` | 真实指令 | `sd rs2, offset(rs1)` | 把 `rs2` 的 8 字节写入 `rs1 + offset` |
| `jalr` | 真实指令 | `jalr rd, offset(rs1)` | 跳到 `rs1 + offset`，并把下一条指令地址写入 `rd` |
| `ecall` | 特权相关指令 | `ecall` | 触发环境调用；用户模式执行时产生陷阱，进入 xv6 内核 |
| `csrrw` | CSR 指令 | `csrrw rd, csr, rs1` | 将 CSR 旧值写入 `rd`，同时把 `rs1` 写入 CSR |
| `sfence.vma` | 内存管理指令 | `sfence.vma rs1, rs2` | 使指定范围的地址转换缓存失效；`zero, zero` 表示整体刷新 |
| `sret` | 特权返回指令 | `sret` | 根据 `sepc`、`sstatus` 等状态从 Supervisor 模式返回 |
| `c.addi` | 真实压缩指令 | `c.addi rd, imm` | 16 位形式；语义类似 `addi rd, rd, imm` |
| `c.addiw` | 真实压缩指令 | `c.addiw rd, imm` | 16 位形式；语义类似 `addiw rd, rd, imm` |
| `c.ldsp` | 真实压缩指令 | `c.ldsp rd, offset(sp)` | 16 位形式；从 `sp + offset` 读取 8 字节 |
| `c.sdsp` | 真实压缩指令 | `c.sdsp rs2, offset(sp)` | 16 位形式；把 8 字节写入 `sp + offset` |
| `c.addi4spn` | 真实压缩指令 | `c.addi4spn rd, sp, imm` | 16 位形式；`rd = sp + imm`，可用于计算栈内地址 |
| `c.li` | 真实压缩指令 | `c.li rd, imm` | 16 位形式；把较小的立即数写入 `rd` |
| `c.jr` | 真实压缩指令 | `c.jr rs1` | 16 位间接跳转；`c.jr ra` 可用于函数返回 |
| `li` | 伪指令 | `li rd, imm` | 加载立即数；根据数值展开为一条或多条真实指令 |
| `ret` | 伪指令 | `ret` | 函数返回；通常展开为 `jalr zero, 0(ra)` |
| `jr` | 伪指令 | `jr rs1` | 跳转且不保存返回地址；展开为 `jalr zero, 0(rs1)` |
| `call` | 伪指令 | `call symbol` | 调用函数；通常由 `auipc` 与 `jalr` 完成 PC 相对跳转 |
| `la` | 伪指令 | `la rd, symbol` | 加载符号地址；常由 `auipc` 加后续指令完成 |
| `csrr` | 伪指令 | `csrr rd, csr` | 读取 CSR，不修改它；展开为 `csrrs rd, csr, zero` |
| `csrw` | 伪指令 | `csrw csr, rs1` | 写入 CSR，不保留旧值；展开为 `csrrw zero, csr, rs1` |

类型说明：

| 类型 | 说明 |
| --- | --- |
| 真实指令 | RISC-V ISA 定义、CPU 可直接执行的普通指令，通常为 32 位 |
| 真实压缩指令 | RISC-V C 扩展定义、CPU 可直接执行的 16 位指令，以 `c.` 开头 |
| 伪指令 | 汇编器提供的便捷写法，汇编时转换为一条或多条真实指令 |

### 2.2 汇编器指示符

下列写法不是 CPU 指令，而是给汇编器或链接器的控制信息。

| 写法                             | 作用                                         |
| ------------------------------ | ------------------------------------------ |
| `.global name` / `.globl name` | 导出符号，使其他目标文件或链接器可以引用                       |
| `.section trampsec`            | 将后续内容放入名为 `trampsec` 的段                    |
| `.align 4`                     | 按汇编器规则对齐后续内容                               |
| `label:`                       | 定义代码地址符号，例如 `sleep:`、`uservec:`、`userret:` |

## 3. 汇编语法与反汇编阅读

### 3.1 内存操作数

`offset(base)` 表示访问地址 `base + offset`，其中偏移量单位是字节。例如：

```asm
sd ra, 40(a0)    # 将 ra 的 8 字节保存到 a0 + 40
ld sp, 48(a0)    # 从 a0 + 48 读取 8 字节到 sp
sw t0, -20(s0)   # 将 t0 的低 4 字节保存到 s0 - 20
lw a0, -20(s0)   # 从 s0 - 20 读取 4 字节并符号扩展
```

### 3.2 阅读 `.asm`

示例：

```asm
34:  600080e7    jalr  1536(ra)    # 630 <printf>
```

| 内容 | 含义 |
| --- | --- |
| `34:` | 指令地址，十六进制表示，即 `0x34` |
| `600080e7` | 机器码，即 CPU 实际执行的指令编码 |
| `jalr` | 指令助记符 |
| `1536(ra)` | 操作数；目标地址由 `ra + 1536` 计算 |
| `# 630 <printf>` | 反汇编器添加的注释；目标是地址 `0x630` 的 `printf` |

执行该 `jalr` 时，CPU 跳转到目标地址，并把下一条指令地址写入 `ra`。如果下一行地址是 `0x38`，执行后便有 `ra = 0x38`。

## 4. xv6 用户态系统调用

`user/usys.pl` 是生成 `user/usys.S` 的 Perl 脚本，并非 RISC-V 汇编文件。例如：

```perl
entry("sleep");
```

会生成：

```asm
.global sleep
sleep:
 li a7, SYS_sleep
 ecall
 ret
```

执行过程：

1. `li` 把 `sleep` 的系统调用号放入 `a7`；它加载的是立即数，不读取内存。
2. `ecall` 产生用户态环境调用陷阱。硬件转到 `stvec` 指定的入口，但不会自动切换页表、切换内核栈或保存全部通用寄存器。
3. xv6 的 `uservec` 保存现场并进入内核；处理结束后通过 `userret` 和 `sret` 回到用户态。
4. 包装函数最后执行 `ret`，根据 `ra` 返回原用户程序的调用点。

## 5. `trampoline.S` 关键指令

`kernel/trampoline.S` 包含两个主要入口：

- `uservec`：从用户态陷入内核时，保存用户现场、切换页表并跳转到 `usertrap()`。
- `userret`：内核处理完成后，切换用户页表、恢复现场并通过 `sret` 返回用户态。

### 5.1 `csrrw`：交换 `a0` 与 `sscratch`

```asm
csrrw a0, sscratch, a0
```

执行效果：

```text
a0       <- 原 sscratch 的值
sscratch <- 原 a0 的值
```

进入 `uservec` 前，`sscratch` 保存当前进程的 `TRAPFRAME` 地址。交换后，`a0` 可作为陷阱帧指针，原用户 `a0` 暂存在 `sscratch`。`userret` 末尾再次交换，用于恢复用户 `a0`，并为下一次陷阱重新准备 `sscratch`。

### 5.2 `sd` 与 `ld`：保存和恢复现场

```asm
sd ra, 40(a0)
sd sp, 48(a0)
sd a7, 168(a0)

ld sp, 8(a0)
ld tp, 32(a0)
ld a7, 168(a0)
```

`uservec` 中的 `a0` 指向当前进程的 `TRAPFRAME`。`sd` 把用户寄存器保存到不同偏移；`ld` 用来读取内核栈、页表、`usertrap` 地址，或在 `userret` 中恢复用户寄存器。

### 5.3 `csrr` 与 `csrw`：读取和写入 CSR

```asm
csrr t0, sscratch
sd   t0, 112(a0)

csrw satp, t1
csrw sscratch, t0
```

`csrr t0, sscratch` 读取原用户 `a0`，随后保存到 `trapframe->a0`。`csrw satp, t1` 切换页表；`csrw sscratch, t0` 为恢复用户 `a0` 做准备。

### 5.4 `sfence.vma`：刷新地址转换状态

```asm
sfence.vma zero, zero
```

修改 `satp` 后，处理器可能仍缓存旧的虚拟地址转换。这里两个操作数均为 `zero`，表示不限定虚拟地址或地址空间标识，执行整体刷新。

### 5.5 `jr`：跳转到 `usertrap()`

```asm
jr t0
```

`jr` 跳转到 `t0` 保存的地址，且不写入新的返回地址。在 `uservec` 中，`t0` 来自 `trapframe->kernel_trap`，因此跳转到 `usertrap()`；该函数不会返回 `uservec`。

### 5.6 `sret`：返回用户模式

`sret` 使用 `sepc` 作为返回地址，并根据 `sstatus.SPP` 决定目标特权级、根据 `sstatus.SPIE` 恢复中断状态。普通系统调用返回路径会清除 `SSTATUS_SPP`，因此返回用户模式；`sepc` 已被设置为 `ecall` 后的下一条指令。

## 6. `uservec` 与 `userret` 流程

### 6.1 `uservec`

```c title:trampoline.S/uservec
.globl trampoline
trampoline:
.align 4
.globl uservec
uservec:    
	#
	# trap.c sets stvec to point here, so
	# traps from user space start here,
	# in supervisor mode, but with a
	# user page table.
	#
	# sscratch points to where the process's p->trapframe is
	# mapped into user space, at TRAPFRAME.
	#
        
	# swap a0 and sscratch
	# so that a0 is TRAPFRAME
	csrrw a0, sscratch, a0

	# save the user registers in TRAPFRAME
	sd ra, 40(a0)
	sd sp, 48(a0)
	sd gp, 56(a0)
	sd tp, 64(a0)
	sd t0, 72(a0)
	sd t1, 80(a0)
	sd t2, 88(a0)
	sd s0, 96(a0)
	sd s1, 104(a0)
	sd a1, 120(a0)
	sd a2, 128(a0)
	sd a3, 136(a0)
	sd a4, 144(a0)
	sd a5, 152(a0)
	sd a6, 160(a0)
	sd a7, 168(a0)
	sd s2, 176(a0)
	sd s3, 184(a0)
	sd s4, 192(a0)
	sd s5, 200(a0)
	sd s6, 208(a0)
	sd s7, 216(a0)
	sd s8, 224(a0)
	sd s9, 232(a0)
	sd s10, 240(a0)
	sd s11, 248(a0)
	sd t3, 256(a0)
	sd t4, 264(a0)
	sd t5, 272(a0)
	sd t6, 280(a0)

	# save the user a0 in p->trapframe->a0
	csrr t0, sscratch
	sd t0, 112(a0)

	# restore kernel stack pointer from p->trapframe->kernel_sp
	ld sp, 8(a0)

	# make tp hold the current hartid, from p->trapframe->kernel_hartid
	ld tp, 32(a0)

	# load the address of usertrap(), p->trapframe->kernel_trap
	ld t0, 16(a0)

	# restore kernel page table from p->trapframe->kernel_satp
	ld t1, 0(a0)
	csrw satp, t1
	sfence.vma zero, zero

	# a0 is no longer valid, since the kernel page
	# table does not specially map p->tf.

	# jump to usertrap(), which does not return
	jr t0
```

| 顺序 | 指令或指令组 | 作用 |
| ---: | --- | --- |
| 1 | `csrrw a0, sscratch, a0` | 取得 `TRAPFRAME` 地址，并暂存用户 `a0` |
| 2 | 多条 `sd ..., offset(a0)` | 将用户通用寄存器保存到 `trapframe` |
| 3 | `csrr t0, sscratch`；`sd t0, 112(a0)` | 保存暂存的用户 `a0` |
| 4 | `ld sp, 8(a0)`；`ld tp, 32(a0)` | 载入内核栈指针和 hart ID |
| 5 | `ld t0, 16(a0)` | 取得 `usertrap()` 地址 |
| 6 | `ld t1, 0(a0)`；`csrw satp, t1` | 切换到内核页表 |
| 7 | `sfence.vma zero, zero` | 刷新地址转换状态 |
| 8 | `jr t0` | 跳转到 `usertrap()` |

### 6.2 `userret`

调用 `userret(TRAPFRAME, pagetable)` 时，`a0` 是陷阱帧地址，`a1` 是用户页表信息。

```c title:trapoline.S/userret
.globl userret
userret:
	# userret(TRAPFRAME, pagetable)
	# switch from kernel to user.
	# usertrapret() calls here.
	# a0: TRAPFRAME, in user page table.
	# a1: user page table, for satp.

	# switch to the user page table.
	csrw satp, a1
	sfence.vma zero, zero

	# put the saved user a0 in sscratch, so we
	# can swap it with our a0 (TRAPFRAME) in the last step.
	ld t0, 112(a0)
	csrw sscratch, t0

	# restore all but a0 from TRAPFRAME
	ld ra, 40(a0)
	ld sp, 48(a0)
	ld gp, 56(a0)
	ld tp, 64(a0)
	ld t0, 72(a0)
	ld t1, 80(a0)
	ld t2, 88(a0)
	ld s0, 96(a0)
	ld s1, 104(a0)
	ld a1, 120(a0)
	ld a2, 128(a0)
	ld a3, 136(a0)
	ld a4, 144(a0)
	ld a5, 152(a0)
	ld a6, 160(a0)
	ld a7, 168(a0)
	ld s2, 176(a0)
	ld s3, 184(a0)
	ld s4, 192(a0)
	ld s5, 200(a0)
	ld s6, 208(a0)
	ld s7, 216(a0)
	ld s8, 224(a0)
	ld s9, 232(a0)
	ld s10, 240(a0)
	ld s11, 248(a0)
	ld t3, 256(a0)
	ld t4, 264(a0)
	ld t5, 272(a0)
	ld t6, 280(a0)

	# restore user a0, and save TRAPFRAME in sscratch
	csrrw a0, sscratch, a0
	
	# return to user mode and user pc.
	# usertrapret() set up sstatus and sepc.
	sret
```

| 顺序 | 指令或指令组 | 作用 |
| ---: | --- | --- |
| 1 | `csrw satp, a1` | 切换回用户页表 |
| 2 | `sfence.vma zero, zero` | 刷新地址转换状态 |
| 3 | `ld t0, 112(a0)`；`csrw sscratch, t0` | 为恢复用户 `a0` 准备 `sscratch` |
| 4 | 多条 `ld ..., offset(a0)` | 恢复除 `a0` 外的用户通用寄存器 |
| 5 | `csrrw a0, sscratch, a0` | 恢复用户 `a0`，并保存下次陷阱所需的 `TRAPFRAME` 地址 |
| 6 | `sret` | 返回用户模式，从 `sepc` 指向的位置继续执行 |

## 7. 栈指针 `sp` 与帧指针 `fp`

下面是便于观察栈帧的示例。`s0` 与 `fp` 均为栈帧寄存器；编译器实际选择的寄存器和偏移可能不同。

### 7.1 C 源代码

`volatile` 使局部变量保留在内存中，让编译器不要做优化，读写操作都在内存进行，不会从`cache`缓存读取。

```c
__attribute__((noinline))
int func(int x)
{
    volatile int offset = 3;          // [F1]
    volatile int result = x + offset; // [F2]
    return result;                     // [F3]
}

__attribute__((noinline))
int caller(int n)
{
    volatile int local = n + 1;       // [C1]
    int value = func(local);           // [C2]
    volatile int answer = value * 2;  // [C3]
    return answer;                     // [C4]
}

int main(void)
{
    return caller(8);                  // [M1]
}
```

### 7.2 对应汇编

```asm
main:
    addi sp, sp, -16
    sd   ra, 8(sp)
    sd   s0, 0(sp)
    addi s0, sp, 16

    li   a0, 8             # [M1] 第一个参数 n = 8
    call caller

    ld   ra, 8(sp)
    ld   s0, 0(sp)
    addi sp, sp, 16
    ret

caller:
    addi sp, sp, -32       # 建立 caller 的 32 字节栈帧
    sd   ra, 24(sp)
    sd   s0, 16(sp)
    addi s0, sp, 32        # fp = 进入 caller 前的 sp

    addiw t0, a0, 1        # [C1] local = n + 1
    sw    t0, -20(s0)

    lw    a0, -20(s0)      # [C2] 第一个参数 x = local
    call  func

    slliw t0, a0, 1        # [C3] answer = value * 2
    sw    t0, -24(s0)
    lw    a0, -24(s0)      # [C4] 返回值放入 a0

    ld   ra, 24(sp)
    ld   s0, 16(sp)
    addi sp, sp, 32
    ret

func:
    addi sp, sp, -32       # 建立 func 的 32 字节栈帧
    sd   ra, 24(sp)
    sd   s0, 16(sp)
    addi s0, sp, 32        # fp = 进入 func 前的 sp

    li    t0, 3            # [F1] offset = 3
    sw    t0, -20(s0)

    lw    t0, -20(s0)
    addw  t0, a0, t0       # [F2] result = x + offset
    sw    t0, -24(s0)
    lw    a0, -24(s0)      # [F3] 返回值放入 a0

    ld   ra, 24(sp)
    ld   s0, 16(sp)
    addi sp, sp, 32
    ret
```

### 7.3 调用 `func` 前后

`caller` 执行 `call func` 前：

```text
高地址

+----------------------+  <- fp(pre)
| caller 保存的 ra     |  fp(pre) - 8
+----------------------+
| caller 保存的旧 fp   |  fp(pre) - 16
+----------------------+
| local                |  fp(pre) - 20
+----------------------+
| answer               |  fp(pre) - 24
+----------------------+
| 预留空间             |
+----------------------+  <- sp = S

低地址
```

`func` 建立栈帧后：

```text
高地址

+----------------------+  <- fp(pre)
| caller 保存的 ra     |
+----------------------+
| caller 保存的旧 fp   |
+----------------------+
| caller 的 local      |
+----------------------+
| caller 的 answer     |
+----------------------+
| caller 的预留空间    |
+----------------------+  <- fp = S
| func 保存的 ra       |  fp - 8
+----------------------+
| func 保存的 fp(pre)  |  fp - 16
+----------------------+
| offset               |  fp - 20
+----------------------+
| result               |  fp - 24
+----------------------+
| func 的预留空间      |
+----------------------+  <- sp = S - 32

低地址（栈向此方向增长）
```

`func` 执行 `ret` 后，`sp` 与 `fp` 恢复到 `caller` 的栈帧：

```text
高地址

+----------------------+  <- fp = fp(pre)
| caller 保存的 ra     |
+----------------------+
| caller 保存的旧 fp   |
+----------------------+
| local                |
+----------------------+
| answer               |
+----------------------+
| 预留空间             |
+----------------------+  <- sp = S

低地址
```

## 8. 相关源码索引

| 文件 | 主要指令 | 主要任务 |
| --- | --- | --- |
| `user/usys.pl` 生成的 `user/usys.S` | `li`、`ecall`、`ret` | 为各系统调用生成用户态包装函数 |
| `kernel/trampoline.S` 的 `uservec` | `csrrw`、`sd`、`csrr`、`ld`、`csrw`、`sfence.vma`、`jr` | 保存用户现场、切换内核页表并进入 `usertrap` |
| `kernel/trampoline.S` 的 `userret` | `csrw`、`sfence.vma`、`ld`、`csrrw`、`sret` | 切换用户页表、恢复现场并返回用户模式 |
| `kernel/trap.c` | — | 处理用户态陷阱和返回准备 |
| `kernel/syscall.c` | — | 根据 `a7` 中的系统调用号分派处理函数 |
