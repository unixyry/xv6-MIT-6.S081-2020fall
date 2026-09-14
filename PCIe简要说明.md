# PCIe 简要说明

PCIe（Peripheral Component Interconnect Express，高速外围组件互连）是连接 CPU（Central Processing Unit，中央处理器）、内存系统与高速外设的通用互连标准。现代计算机中的显卡、高速网卡、NVMe（Non-Volatile Memory Express，非易失性存储器高速接口）SSD（Solid-State Drive，固态硬盘）和各类加速卡，通常都通过 PCIe 接入系统。

PCIe 不只是“寻找设备”。设备发现只发生在初始化阶段；系统运行时，处理器访问设备寄存器、设备直接访问主内存、发送中断，以及链路错误和电源管理，也都依赖 PCIe。严格来说，PCIe 是点对点、分层并可交换的互连网络，而不是传统的共享电气总线，不过工程中仍习惯称它为“PCIe 总线”。

## 1. PCIe 在系统中的位置

一个较接近实际开发环境的拓扑如下：

```text
                              CPU 与内存
                                  │
                         PCIe Root Complex
                         （PCIe 根复合体）
                                  │
                    ┌─────────────┴─────────────┐
                 Root Port                   Root Port
                    │                            │
              PCIe 显卡                     PCIe Switch
       ┌────────────┴────────────┐        ┌──────┴────────┐
   Function 0                Function 1  高速网卡       NVMe SSD
   图形控制器                显示音频
```

Root Complex 连接处理器/内存系统和 PCIe 层次结构，Root Port 和 PCIe Switch 则把拓扑扩展到更多设备。传统 SATA（Serial Advanced Technology Attachment，串行高级技术附件）硬盘通常不是 PCIe 设备：PCIe 发现的是 AHCI（Advanced Host Controller Interface，高级主机控制器接口）控制器，硬盘位于该控制器后面的 SATA 链路上；NVMe SSD 通常才是直接连接 PCIe 的设备。

PCIe 主要提供以下能力：

1. 建立物理链路，协商链路速率和宽度，例如第 4 代 x4 或第 5 代 x16。
2. 提供统一配置空间，使固件或操作系统能够枚举和识别设备。
3. 通过 BAR（Base Address Register，基地址寄存器）给设备分配处理器可访问的地址窗口，即 MMIO（Memory-Mapped Input/Output，内存映射输入/输出）。
4. 承载设备发起的 DMA（Direct Memory Access，直接内存访问）事务。
5. 传递 MSI（Message Signaled Interrupt，消息信号中断）及 MSI-X（MSI 的扩展形式）。
6. 支持错误报告、电源管理、热插拔和虚拟化等能力。

## 2. MMIO、DMA 与 PCIe 的关系

设备驱动主要通过 MMIO 控制设备，通过 DMA 传输大量数据：

```text
控制路径（MMIO）

CPU 执行 load/store
        │
        ▼
Root Complex 将地址访问转换为 PCIe 事务
        │
        ▼
根据设备 BAR 路由到目标设备
        │
        ▼
设备内部控制寄存器


数据路径（DMA）

设备内部 DMA 引擎
        │  发起 PCIe Memory Read / Memory Write
        ▼
      PCIe
        │
        ▼
IOMMU（Input-Output Memory Management Unit，输入/输出内存管理单元，可选）
        │  地址转换与访问权限检查
        ▼
      主内存
```

因此，“DMA 由设备控制器负责”和“DMA 通过 PCIe 完成”并不矛盾：DMA 引擎位于设备内部，决定何时读写哪个地址；PCIe 负责承载和路由这些内存读写事务。最终的内存访问由 Root Complex、IOMMU 和内存系统共同完成。

例如网卡接收数据时，网卡的 DMA 引擎先通过 PCIe 读取接收描述符，再把数据包写入描述符指定的主内存，最后写回完成状态并发送中断。CPU 不需要逐字节搬运数据。

## 3. BDF 与 PCIe 配置空间

BDF（Bus/Device/Function，总线/设备/功能）是 PCIe 配置空间中一个功能的逻辑地址。Linux 中常见的完整形式为：

```text
Segment : Bus : Device . Function
  0000  : 03  : 00     . 1
```

- Segment：彼此独立的 PCIe 配置域，一台大型服务器可能有多个。
- Bus：一个逻辑总线编号；根总线以及每座桥后面的总线通常有不同编号。
- Device：某条 Bus 上的设备位置，传统编号范围为 0～31。
- Function：一个物理设备暴露的逻辑功能，传统编号范围为 0～7。例如显卡的图形控制器和 显示音频可以是同一 Device 的两个 Function。

每个 Function 都有独立的 PCIe 配置空间，开头包含：

```text
偏移 0x00：Vendor ID（Vendor Identifier，厂商标识符）和 Device ID（Device Identifier，设备标识符）
偏移 0x04：Command（命令）和 Status（状态）
偏移 0x08：Class Code（类别编码）和 Revision ID（Revision Identifier，修订标识符）
偏移 0x0c：Header Type（配置头类型）等
偏移 0x10：BAR0
偏移 0x14：BAR1
...
```

PCIe 通常通过 ECAM（Enhanced Configuration Access Mechanism，增强配置访问机制）把配置空间映射到处理器地址空间。一个 Function 占 4 KiB，因此其地址大致为：

```text
ECAM_BASE
  + (Bus      << 20)
  + (Device   << 15)
  + (Function << 12)
  + 配置寄存器偏移
```

## 4. 系统如何枚举设备

枚举不是 PCIe 自己主动搜索，而是固件或操作系统按照 PCIe 规则读取配置空间。系统首先从平台固件获得 Root Complex、ECAM 基地址和可用 Bus 范围；实际机器常通过 ACPI（Advanced Configuration and Power Interface，高级配置与电源接口）或设备树描述这些信息。

```text
从每个 Root Bus 开始
          │
          ▼
依次检查 Device 0～31 的 Function 0
          │
          ├── Vendor ID == 0xffff ──► 此位置无设备，继续下一个 Device
          │
          └── Vendor ID 有效 ───────► 发现一个 Function
                                      │
                                      ├─ 读取 Device ID、Class Code、BAR 和能力
                                      ├─ 若为多功能设备，检查 Function 1～7
                                      └─ 若为 PCIe Bridge
                                             │
                                             ▼
                                      扫描其 Secondary Bus
```

目标 BDF 没有设备响应时，配置读取通常返回全 1，因此 `Vendor ID == 0xffff` 被当作“不存在”。读到合法 Vendor ID 才算发现一个 PCI Function，但这还不等于设备已经可用：系统还要分配资源、配置中断、启用设备并绑定相应驱动。

一条 Bus 上的设备编号可以有空洞，所以遇到一个 `0xffff` 不能停止，仍需检查到 Device 31。若发现 PCIe Bridge，则继续递归扫描桥后的 Bus。枚举在所有 Root Bus 以及所有可达桥后的 Bus、Device、Function 都检查完后结束；支持热插拔的系统还会在运行期间局部重新枚举。

## 5. BAR 的含义与配置

BAR 位于设备的 PCIe 配置空间中，用来建立“处理器地址范围”与“设备内部资源”之间的映射。设备通过 BAR 表达所需窗口的类型、大小和属性，固件或操作系统选择无冲突且满足对齐要求的基地址，再把它写回 BAR。

```text
设备配置空间                         CPU 地址空间

BAR0 = 0x8000_0000  ─────────────►  0x8000_0000  设备控制寄存器 0
                                    0x8000_0004  设备控制寄存器 1
                                    ...
                                    0x8001_ffff  BAR0 窗口末尾

CPU 访问 0x8000_0100
        │
        ▼
Root Complex 判断地址属于该 BAR
        │
        ▼
把访问转发给设备，设备看到窗口内偏移 0x100
```

BAR 可以映射设备控制寄存器、显存窗口或板载内存等资源。它不是 DMA 缓冲区地址：BAR 解决的是“CPU 如何访问设备”，DMA 地址解决的是“设备如何访问主内存”。

传统的 BAR 配置过程是保存原值、写入全 1、读回硬件实现的掩码以计算大小，然后分配地址并写回。BAR 可能表示内存空间或传统端口输入/输出空间，也可能是 32 位或 64 位；一个 64 位 BAR 会占用两个连续的 BAR 寄存器。

## 6. 与 xv6 示例的区别

xv6 的 [`pci_init()`](../xv6-labs-2020/kernel/pci.c) 是面向固定 QEMU 环境的教学实现：它只检查 Bus 0 和 Function 0，只识别指定的 E1000 网卡，并把 BAR0 直接写成固定地址 `0x40000000`。它没有实现真实系统中的桥递归、多功能设备扫描、通用 BAR 资源分配和完整中断配置。

这个例子仍然展示了 PCIe 的关键分工：先通过配置空间找到并启用设备，再通过 BAR 建立 MMIO 窗口；运行时 CPU 的 MMIO、网卡发起的 DMA，以及设备中断，都继续依赖 PCIe 通信。
