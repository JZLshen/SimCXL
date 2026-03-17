# 本地代码相较远端最新代码的增量说明

## 1. 文档目的

本文档用于回答两个问题：

1. 当前本地代码相较于远端最新代码，究竟新增了哪些内容。
2. 这些新增内容分别起什么作用，为什么它们对当前项目是必要的。

本文档只关注与 SimCXL 项目本身直接相关的代码、脚本、实验和支撑补丁，不覆盖整个 gem5 上游通用代码树的所有细枝末节。

## 2. 比较基线

本地在 2026-03-11 对远端执行过 `git fetch --all --prune`，当时各基线为：

- 本地 `HEAD`: `e64a7203c1` (`add exp and debug`)
- `origin/main`: `5908b5857c` (`2026-01-12 Update README.`)
- `upstream/main`: `21176cf4ab` (`2026-03-04 Update README`)

结论如下：

- 相对 `origin/main`，本地领先 4 个提交，落后 0 个提交。
- 相对 `upstream/main`，本地领先 4 个提交，落后 1 个提交。
- 本地相对 `upstream/main` 的落后部分只有一个 README 更新提交，因此从“代码逻辑”的角度看，本地与远端最新代码的真实差异，仍然基本等价于“本地相对 `origin/main` 的 4 个新增提交”。

因此，本文档默认以 `origin/main` 作为代码比较基线，同时补充说明 `upstream/main` 的差异只涉及 README，不影响代码判断。

## 3. 总体差异概览

### 3.1 已提交到本地分支的增量

相对 `origin/main..HEAD`：

- 共 `60` 个文件发生变化
- 共 `14518` 行新增
- 共 `105` 行删除

本地新增提交共 4 个：

1. `4475a71f4b` `cxl-rpc-v1`
2. `6d78eb3f91` `redesign metatdata bit and req_data and resp_data`
3. `d92192d0c6` `edit client send1-poll1 and add exp.sh`
4. `e64a7203c1` `add exp and debug`

### 3.2 当前 worktree 中尚未提交的增量

当前工作区相对本地 `HEAD` 还有一层未提交改动：

- 8 个已跟踪文件继续被修改
- `271` 行新增
- `57` 行删除
- 另有一批未跟踪文件，包括中文文档、CPU memmove 实验脚本、实验原始日志、图表、镜像与论文资料

这意味着“当前本地代码”实际上分成两层：

- 第一层：已经提交在 `main` 分支上的 4 个本地提交
- 第二层：还没有提交、但已经存在于工作区的实验/诊断增强

## 4. 本地增量的核心结论

相对远端基线，本地代码最重要的新增不是零散 patch，而是围绕以下 5 条主线形成的闭环：

1. **完整的 CXL-RPC 栈**
   从 simulator 侧 doorbell 拦截，到 guest 侧共享内存 RPC 库，再到 client/server 示例程序与实验脚本，形成了可运行的 CPU-to-CPU RPC 路径。

2. **CXL doorbell 到 metadata queue 的重映射机制**
   RPC 不是简单在 CXL memory 上做 load/store，而是通过控制器在运行时把 doorbell 写重写为 metadata queue 写，从而复用了现有内存系统并加入协议语义。

3. **CopyEngine 响应异步 offload**
   server 端返回响应时，不是只靠 CPU memcpy，而是可以把 response payload 和 completion flag 交给 CopyEngine lane 异步写回 client 侧共享区。

4. **KVM 启动、checkpoint、恢复、切换 TIMING/O3 的稳定工作流**
   本地不仅加了 RPC 配置脚本，还为支撑这一流程修了 x86/KVM、MSR、CPUID、event thread 和 `kick()` 相关补丁。

5. **完整实验体系**
   包括 guest 二进制注入、矩阵批跑、日志提取、trace 对齐、DMA 带宽 sweep、`XFERCAP` sweep，以及当前未提交的 CPU `memmove` sweep。

## 5. 远端基线本来就提供什么

在说明本地新增之前，先明确远端基线已经具备的内容：

- SimCXL 的 Type-3 CXL memory device
- Type-1 accelerator 与 Type-2 accelerator
- Classic 路径与 Ruby 路径的整机装配
- 专用的 Ruby CXL coherence protocol

也就是说，本地工作并不是从零开始做 CXL 模型，而是在已有 SimCXL 基线上，继续叠加：

- CXL-RPC
- CopyEngine 响应下发
- KVM/checkpoint 工作流
- guest 程序与实验工具链

## 6. 4 个已提交本地提交的作用

### 6.1 `4475a71f4b` `cxl-rpc-v1`

这是本地增量中最关键的基础提交，主要完成以下内容：

- 新增 RPC 专用配置脚本
- 新增 `CXLRPCEngine`
- 扩展 `CXLMemCtrl` 让其具备 RPC doorbell 处理能力
- 扩展 board 装配，把 RPC engine、CopyEngine 和 CXL memory 接到同一个系统里
- 新增 guest 侧 `libcxlrpc`
- 新增 guest 侧 client/server 示例
- 新增 guest 侧 CXL memory 与 CopyEngine 基准程序
- 新增 disk image 注入、运行脚本、trace 解析工具
- 为门铃重映射和 CopyEngine/KVM 路径补齐一批基础设施 patch

一句话概括：这个提交第一次把“SimCXL Type-3 shared memory”升级成“可跑完整共享内存 RPC 协议”的系统。

### 6.2 `6d78eb3f91` `redesign metatdata bit and req_data and resp_data`

这个提交主要是在已有 RPC 栈上做协议与布局重构，重点包括：

- 重新设计 metadata entry 的 phase bit 语义
- 重新整理 request_data / response_data 的布局与发布/消费方式
- 改进连接初始化与固定地址模式
- 改进 server/client 对 request/response 的解释
- 更新矩阵实验脚本和注入脚本以适配新的数据布局

一句话概括：这个提交把 RPC v1 从“能跑”推进到“协议语义更加稳定、数据结构更清楚”的状态。

### 6.3 `d92192d0c6` `edit client send1-poll1 and add exp.sh`

这个提交主要调整 client 侧发送/轮询节奏和实验脚本：

- client 的请求调度从更早的方案收敛到更明确的 `send/poll` 行为
- `libcxlrpc` 中的请求/完成队列处理继续细化
- 批量实验脚本继续补强

一句话概括：这个提交主要解决的是 client 运行时行为和实验自动化的可控性。

### 6.4 `e64a7203c1` `add exp and debug`

这个提交主要是实验、调试和文档增强：

- 新增 DMA sweep 实验目录 `exp/DMA`
- 新增 DMA 结果提取与绘图脚本
- 新增 RPC 测试 README
- 增加更多调试输出和 debug 统计
- 完善 RPC 测试配置脚本

一句话概括：这个提交把“功能实现”进一步扩展成“可系统测量、可复现实验、可分析结果”的工程体系。

## 7. 已提交本地增量：按子系统详细罗列

下面按子系统列出本地相对远端基线新增或修改的代码，以及它们的作用。

### 7.1 配置脚本与整机装配

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `configs/example/gem5_library/x86-cxl-rpc-test.py` | 新增 | RPC 主测试配置脚本。负责 KVM 启动、checkpoint 恢复、CPU 类型切换、RPC client 数量推断、guest 命令注入。 |
| `configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py` | 新增 | 先用 KVM 启动系统，再在合适时机保存 checkpoint，供后续 TIMING/O3/RPC 实验恢复。 |
| `src/python/gem5/components/boards/x86_board.py` | 修改 | 这是本地最核心的系统装配入口。新增 CXL device、CopyEngine 组、RPC engine、doorbell range、E820 CXL memory 暴露、Classic/Ruby 分支下的不同接线。 |
| `src/dev/x86/CXLDevice.py` | 修改 | 给 `CXLMemCtrl` 增加可选的 `rpc_engine` 参数，使控制器能挂上 RPC 引擎。 |
| `src/dev/x86/CXLRPCEngine.py` | 新增 | Python SimObject 声明，定义 `CXLRPCEngine` 参数与对象类型。 |
| `src/dev/x86/SConscript` | 修改 | 把 RPC engine 源文件纳入构建系统。 |

这部分代码的作用是：把原本只有 CXL memory 的 Type-3 板级系统，升级成“CXL memory + RPC engine + CopyEngine + checkpoint 工作流”的完整实验平台。

### 7.2 `CXLMemCtrl` 与 RPC simulator 热路径

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `src/dev/x86/cxl_mem_ctrl.hh` | 修改 | 扩展控制器接口，增加对 RPC engine 的持有和门铃处理相关声明。 |
| `src/dev/x86/cxl_mem_ctrl.cc` | 修改 | 在请求接收路径中引入 doorbell probe、control bypass、request remap、queue full retry、internal consume 等逻辑。 |
| `src/dev/x86/cxl_rpc_engine.hh` | 新增 | 定义 doorbell entry、metadata queue、客户端连接表、控制操作、统计项和地址翻译状态。 |
| `src/dev/x86/cxl_rpc_engine.cc` | 新增 | 实现门铃解析、request remap、HEAD_UPDATE 消费、控制门铃处理、地址翻译学习、连接注册与统计。 |

这部分代码构成了本地 RPC 的 simulator 侧核心：

- client 写 doorbell
- `CXLMemCtrl` 决定是否交给 `CXLRPCEngine`
- `CXLRPCEngine` 把 request doorbell 改写成 metadata queue entry 写
- server 端通过 metadata queue 消费请求
- server 发回 `HEAD_UPDATE` 或 response

### 7.3 为门铃重映射补齐的通用内存系统 patch

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `src/mem/packet.hh` | 修改 | 把 `isMaskedWrite()` 从“仅 `WriteReq`”放宽到“任意 `isWrite()` 且带 byte enable”，使 remapped metadata 写能够用 writeback 类包表达。 |
| `src/mem/abstract_mem.cc` | 修改 | 不再把带 mask 的 `WritebackClean` 视为普通 clean evict 直接忽略，使 metadata queue 写真正落到内存。 |
| `src/mem/cache/base.cc` | 修改 | 允许某些 uncacheable response 没有 `senderState` 时直接回 CPU，避免 RPC/KVM 特殊路径在 cache 层触发 panic。 |

这几处 patch 的意义在于：本地 RPC 不是一个孤立设备模型，而是要求 gem5 的通用 packet/memory/cache 路径也能承载“按字节掩码更新 metadata queue”的语义。

### 7.4 CopyEngine、DMA、PCI 支撑 patch

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `src/dev/dma_device.hh` | 修改 | 增加 `allowUnconnectedDefaultDmaPort()`，允许某些 DMA 设备不连接默认标量 DMA 口。 |
| `src/dev/dma_device.cc` | 修改 | 在 `init()` 中放宽默认 DMA 端口强制连接约束。 |
| `src/dev/pci/copy_engine.hh` | 修改 | 对 CopyEngine 覆盖 `allowUnconnectedDefaultDmaPort()`，因为它实际只使用按通道划分的 DMA port。 |
| `src/dev/pci/device.cc` | 修改 | 修复 PCI config space 中 `PCI_COMMAND`/`PCI_STATUS` 的写入宽度，避免 16/32 位写被错误截断。 |

这部分改动的直接作用是：

- guest 能正确枚举并启用 CopyEngine 的 BAR 与 bus mastering
- CopyEngine 在 gem5 中不会因为默认 DMA 口未连接而初始化失败

### 7.5 Classic cache/IO 层的小补丁

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `src/python/gem5/components/cachehierarchies/classic/private_l1_private_l2_shared_l3_cache_hierarchy.py` | 修改 | 只把真实 DRAM 范围交给 iocache，避免将 PCI/MMIO hole 错误纳入 cache 覆盖范围。 |

这是一个小改动，但它能减少 Classic 路径下 IO 地址空间处理的歧义。

### 7.6 KVM / checkpoint 稳定性补丁

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `src/arch/x86/kvm/x86_cpu.cc` | 修改 | 修复 APICBASE.BSP 同步、MSR 同步缺失项处理、CR8 写回、MSR intersection 缓存等问题。 |
| `src/arch/x86/kvm/x86_cpu.hh` | 修改 | 给 MSR intersection 缓存补锁和有效位。 |
| `src/cpu/kvm/base.cc` | 修改 | 跟踪 vCPU event-queue thread 有效性，改进 `kick()`、drain/resume、fork 后状态恢复。 |
| `src/cpu/kvm/base.hh` | 修改 | 为 `kick()` 和线程有效性新增成员与声明。 |
| `src/cpu/kvm/vm.cc` | 修改 | 把支持的 CPUID/MSR 列表缓存改为线程安全一次性初始化。 |
| `src/cpu/kvm/vm.hh` | 修改 | 给 CPUID/MSR cache 增加 `std::once_flag`。 |

这些补丁的作用是支撑当前实验工作流：

- 先 KVM 快速启动
- 保存 checkpoint
- 恢复后切 TIMING/O3
- 继续执行 RPC 与 CopyEngine 实验

### 7.7 guest 侧 `libcxlrpc` 库

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `tests/test-progs/lib/libcxlrpc/Makefile` | 新增 | 独立构建 guest 侧 RPC 静态库。 |
| `tests/test-progs/lib/libcxlrpc/include/cxl_rpc.h` | 新增 | 定义 RPC 对外 API，包括连接创建、发送请求、轮询请求、发送响应、CopyEngine lane 绑定等。 |
| `tests/test-progs/lib/libcxlrpc/include/cxl_alloc.h` | 新增 | 定义连接地址分配器 API。 |
| `tests/test-progs/lib/libcxlrpc/include/cxl_sync.h` | 新增 | 定义 HEAD_UPDATE 同步器 API。 |
| `tests/test-progs/lib/libcxlrpc/include/cxl_timing.h` | 新增 | 提供 guest 侧时间/统计辅助定义。 |
| `tests/test-progs/lib/libcxlrpc/src/cxl_rpc_internal.h` | 新增 | 定义内部上下文、连接、CopyEngine 状态、request/response ring 状态。 |
| `tests/test-progs/lib/libcxlrpc/src/cxl_rpc.c` | 新增 | 实现 shared-memory 初始化、固定地址连接、request/response API、metadata queue 轮询、response 消费、peer response 区配置。 |
| `tests/test-progs/lib/libcxlrpc/src/cxl_alloc.c` | 新增 | 实现全局 allocator。一次性分配 per-connection 连续空间，再切 doorbell/mq/request/response/flag。 |
| `tests/test-progs/lib/libcxlrpc/src/cxl_sync.c` | 新增 | 实现 server 端按固定阈值发送 `HEAD_UPDATE`。 |
| `tests/test-progs/lib/libcxlrpc/src/cxl_copyengine.c` | 新增 | 实现 guest 侧 CopyEngine BAR 扫描、lane 绑定、descriptor pool、response async offload。 |

这是本地最重要的 guest 侧代码。没有这一层，simulator 侧的 RPC engine 没有对应的软件协议端点。

### 7.8 guest 侧 RPC / CXL / CopyEngine 程序

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `tests/test-progs/cxl-rpc/Makefile` | 新增 | 编译所有 RPC/CXL/CopyEngine 基准与示例程序。 |
| `tests/test-progs/cxl-rpc/README.md` | 新增 | 说明 RPC API 语义、运行方式和实验工作流。 |
| `tests/test-progs/cxl-rpc/src/rpc_client_example.c` | 新增 | RPC client 示例，负责发请求、轮询响应、输出每个请求的 tick。 |
| `tests/test-progs/cxl-rpc/src/rpc_server_example.c` | 新增 | RPC server 示例，负责轮询 metadata queue、解析请求、通过 CopyEngine 返回响应。 |
| `tests/test-progs/cxl-rpc/src/cxl_mem_ldst_latency.c` | 新增 | 测 CXL shared memory 上 raw store/load、flush 后 load、roundtrip 等延迟。 |
| `tests/test-progs/cxl-rpc/src/cxl_mem_paper_latency.c` | 新增 | 做“paper-style dependent random read”延迟测量，对比 CXL 区与本地内存。 |
| `tests/test-progs/cxl-rpc/src/cxl_mem_copy_cmp.c` | 新增 | 比较 memcpy 与 CopyEngine 完整提交+完成路径的时间开销。 |
| `tests/test-progs/cxl-rpc/src/cxl_copyengine_sanity.c` | 新增 | 验证 CopyEngine response 路径是否工作正确。 |
| `tests/test-progs/cxl-rpc/src/cxl_copyengine_bw.c` | 新增 | 大规模 CopyEngine 带宽测试程序，可测多 engine、多 channel、不同 `XFERCAP`。 |

这一组程序构成了当前项目的 guest 侧“功能验证 + 性能评估”主体。

### 7.9 运行脚本、镜像注入与 trace 工具

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `tests/test-progs/cxl-rpc/scripts/run_rpc_test.sh` | 新增 | 手工入口脚本，可保存 checkpoint、运行 server-client、运行 benchmark。 |
| `tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py` | 新增 | 批量矩阵实验主控脚本；按实验点重建 checkpoint，恢复到 TIMING 运行，并抽取 tick 结果。 |
| `tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh` | 新增 | 构建 guest 程序、挂载 disk image、拷贝二进制、安装 `m5` 和 `gem5-readfile.service`。 |
| `tools/parse_crosscore_cxl_trace.py` | 新增 | 从 gem5 trace 中提取跨核心 CXL RPC 时序，优先使用 RPC remap 日志对齐。 |
| `tests/.gitignore` | 修改 | 忽略新增测试构建产物。 |

这部分代码让项目从“能编译”变成“能跑整套实验并自动出结果”。

### 7.10 DMA 实验目录

| 文件 | 状态 | 作用 |
| --- | --- | --- |
| `exp/DMA/src/run_dma_matrix.sh` | 新增 | 跑多 engine / 多 channel 带宽矩阵。 |
| `exp/DMA/src/run_dma_xfercap_sweep.sh` | 新增 | 跑单 engine 单 channel 的 `XFERCAP` sweep。 |
| `exp/DMA/src/extract_and_plot_dma.py` | 新增 | 从日志中抽取 DMA sweep 结果并画图。 |
| `exp/DMA/src/extract_and_plot_dma_xfercap.py` | 新增 | 从日志中抽取 `XFERCAP` sweep 结果并画图。 |
| `exp/DMA/src/cxl_copyengine_bw.c` | 新增 | 指向 benchmark 源码的实验目录入口。 |
| `exp/DMA/src/x86-cxl-rpc-test.py` | 新增 | 指向 gem5 启动脚本的实验目录入口。 |
| `exp/DMA/data/engine_sweep.csv` | 新增 | 保存多 engine sweep 数据。 |
| `exp/DMA/data/channel_sweep.csv` | 新增 | 保存多 channel sweep 数据。 |
| `exp/DMA/data/xfercap_sweep.csv` | 新增 | 保存 `XFERCAP` sweep 数据。 |
| `exp/DMA/images/engine_sweep.svg` | 新增 | 对应实验图表。 |
| `exp/DMA/images/channel_sweep.svg` | 新增 | 对应实验图表。 |
| `exp/DMA/images/xfercap_sweep.svg` | 新增 | 对应实验图表。 |

这部分内容说明本地仓库已经从“功能实现”走到了“结果沉淀和论文/报告支撑”的状态。

## 8. 当前未提交到本地分支的增量

下面这些不是远端基线的一部分，也还没有提交到本地 `main` 分支，但它们已经存在于当前工作区，因此也属于“当前本地代码”的一部分。

### 8.1 当前已跟踪文件上的继续修改

| 文件 | 当前 worktree 增量 | 作用 |
| --- | --- | --- |
| `README.md` | 修改 | 本地文档继续更新。 |
| `tests/.gitignore` | 修改 | 增补新的测试产物忽略规则。 |
| `tests/test-progs/cxl-rpc/Makefile` | 修改 | 新增 `cpu_memmove_bw` 目标。 |
| `tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py` | 修改 | 增加 `--only-exp-id`，用于只运行某一个实验点。 |
| `tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh` | 修改 | 增加 `cpu_memmove_bw` 注入、`CXL_RPC_DIAG_LOG`、`rpc_stage=` 阶段标记、串口日志 tee。 |
| `tests/test-progs/cxl-rpc/src/cxl_copyengine_sanity.c` | 修改 | 跟随新 API，切换到 lane bind + peer response data/flag + 新的 consume 语义。 |
| `tests/test-progs/cxl-rpc/src/rpc_client_example.c` | 修改 | 增加 client 侧诊断日志。 |
| `tests/test-progs/cxl-rpc/src/rpc_server_example.c` | 修改 | 增加 server 侧诊断日志。 |

这层增量的本质是：在已提交 RPC 主干之上，继续增强实验可观测性和 CPU `memmove` 对照实验能力。

### 8.2 当前未跟踪但与代码直接相关的文件

| 文件 | 类型 | 作用 |
| --- | --- | --- |
| `README_zh.md` | 文档 | 中文版项目说明。 |
| `exp/DMA/README.md` | 文档 | 总结 DMA 与 CPU memmove 四类实验。 |
| `tests/test-progs/cxl-rpc/src/cpu_memmove_bw.c` | 新程序 | guest 侧 CPU `memmove` 带宽基准，用于与 CopyEngine 带宽做对照。 |
| `exp/DMA/src/cpu_memmove_bw.c` | 实验入口 | 指向 CPU `memmove` benchmark 源。 |
| `exp/DMA/src/run_cpu_memmove_sweep.sh` | 新脚本 | 批量运行 CPU `memmove` core sweep。 |
| `exp/DMA/src/extract_and_plot_cpu_memmove.py` | 新脚本 | 从原始日志抽取 CPU `memmove` 结果并绘图。 |
| `exp/DMA/data/cpu_memmove_sweep.csv` | 数据 | CPU `memmove` sweep 结果。 |
| `exp/DMA/images/cpu_memmove_sweep.svg` | 图表 | CPU `memmove` sweep 图。 |
| `tests/test-progs/cxl-rpc/cpu_memmove_bw` | 构建产物 | 编译得到的 guest 二进制。 |

### 8.3 当前未跟踪但主要属于实验资产或环境资产的文件

这些内容属于“当前本地环境”而不是核心代码逻辑，但它们支撑实验复现：

- `exp/DMA/data/raw/...`
  保存大量 experiment raw logs，包括 `board.pc.com_1.device` 与 `host.log`。
- `files/parsec.img`、`files/vmlinux`、`files/md5`
  实验镜像与内核资产。
- `paper/...`
  参考论文与笔记材料。

它们不是代码逻辑的一部分，但对复现实验流程和结果分析是有价值的。

## 9. 从能力角度看，本地到底比远端多了什么

如果用“能力列表”而不是“文件列表”来概括，本地相对远端基线新增了以下能力：

### 9.1 远端基线没有的系统能力

- 在 Type-3 shared memory 之上运行一套完整的 CPU-to-CPU CXL-RPC 协议
- 在 `CXLMemCtrl` 中拦截 doorbell，并重映射成 metadata queue entry 写
- 让 server 侧 response 通过 CopyEngine lane 异步写回 client
- 基于 KVM 快速启动系统，再切 TIMING/O3 跑详细实验
- 自动保存 checkpoint、自动恢复 checkpoint、自动推导 CopyEngine 拓扑
- 自动构建 guest 程序并注入镜像
- 自动从串口/board log 抽取 per-request tick
- 自动解析 gem5 trace 中的 RPC 跨核心时序
- 批量运行 DMA 矩阵实验并产出 CSV/SVG

### 9.2 当前工作区继续新增但尚未提交的能力

- 更细粒度的诊断日志开关 `CXL_RPC_DIAG_LOG`
- `rpc_stage=` 阶段标记，便于从串口日志中识别运行进度
- `--only-exp-id` 这样的实验调度能力
- CPU `memmove` benchmark 与 sweep 体系，作为 CopyEngine 的对照组

## 10. 结论

相对远端最新代码，本地代码的有效增量可以概括为：

- 远端提供了 SimCXL 的设备模型和基础整机装配
- 本地新增了完整的 CXL-RPC 系统、CopyEngine 响应 offload、KVM/checkpoint 工作流、guest 侧库与程序，以及实验自动化和结果分析工具
- 当前 worktree 又在此基础上继续叠加了诊断日志和 CPU `memmove` 对照实验

从工程价值上看，本地已经不只是“在 SimCXL 上打几个 patch”，而是把仓库扩展成了一个能够：

- 建模 CXL shared-memory RPC
- 自动运行实验
- 输出结构化性能结果
- 支撑进一步论文/报告工作的实验平台

这也是当前本地代码相较远端基线最核心的意义。
