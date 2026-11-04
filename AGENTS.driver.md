# AGENTS.driver.md — 驱动 / 内核模块 / hwbp / 隐蔽性（AGENTS.md 分册）

> 2026-11-04 从 AGENTS.md 拆出（原 50 条/963 行，见 BACKLOG P1-15）。
> **编号沿用主文件历史顺序，不重排**（条目间互引"见第 N 条"依赖原编号）。
> 跨文件引用请回主文件《已知债索引》查条目所在分册。

4. **rwproc33 崩内核的四点修复（2026-10-22）—— 现状见第 27 条**。
   根因在驱动侧：`__pa(si.totalram)` 当物理地址上界、`get_proc_phy_addr` 不判 pmd 大页、
   `__va()` 后无 pfn 有效性守卫、页表遍历在 `mmap_lock` 外。四处都改在
   `build/rwProcMem33` 源码里。
   ⚠️ 第四点「加守卫」当时的实现写错了地址基准（`PHYS_PFN(cur - PAGE_OFFSET)` 少减
   `memstart_addr`），反而引入"部分页静默丢弃"的回归；真因分析与修法见第 27 条。
   「不能用 totalram 当上界」这条本身成立：真机实测合法 paddr 可达 0xaffffefff
   （末端 bank），而 RAM 只有 11.6GB（PA_BITS=48）。

20. **两个内核模块都改过名**（2026-10-22 改 hwbp，2026-10-23 改内存模块）。
   改法一致：模块目录的 Makefile 换 `obj-m`，源文件名保持不变。

   | 原模块名 | 现模块名 | 产物 | 改法 |
   |---|---|---|---|
   | `hwBreakpointProc_module` | **rwNova** | `rwNova_6.12.69.ko` | `obj-m += rwNova.o` / `rwNova-y := hwBreakpointProc_module.o` |
   | `rwProcMem_module` | **rwNova33** | `rwNova33_6.12.69.ko` | `obj-m += rwNova33.o` / `rwNova33-y := rwProcMem_module.o` |

   **两个名字不能相同** —— 同名模块第二个 `insmod` 直接 `-EEXIST`。

   ⚠️ **`build/rwProcMem33/` 是上游克隆，本身不入库**（`.gitignore` 的 `build/*`，它带自己的
   `.git`、未推送提交和本地修复，`git add` 会把它当 gitlink）。**源码快照已入库 `ko/rwProcMem33/`**
   （2026-10-23，184 个源文件），`obj-m` 改名也已在快照里；两边漂移由
   `scripts/check_ko_src_sync.py` 发现，改完工作副本用 `scripts/sync_ko_src.py` 回写快照。
   换机器/重新 clone 后：用 `ko/rwProcMem33/` 的源码，或把它 cp 到 `build/rwProcMem33/`
   （编译脚本按后者路径找文件）。反向（快照→工作副本）没有脚本，手动 cp 前先
   `git -C build/rwProcMem33 status` 看清会不会覆盖本地修改。
   - `build/rwProcMem33/rwProcMem33Module/rwProcMem_module/Makefile`：
     `obj-m += rwNova33.o` / `rwNova33-y := rwProcMem_module.o`
   - 编出的 `rwNova33.ko` → 放 `build/ko/rwNova33_<内核版本>.ko`
   编译要用 `/root/gki`（有完整 `modules` 目标），且必须跑
   `build/ko/_dev/fix_modc.py` 修 `__version_ext_names[] = ;` 那个上游缺陷，
   见 RWPROC33_ADAPTER.md §10.4.1。
   **proc 节点名不受影响**：它来自 `CONFIG_PROC_NODE_AUTH_KEY`
   （内存 `e84523…` / 断点 `dce377…`），跟模块名无关，所以 agentd 的探测路径照旧。

   原来
   `build_hwbp_ko.sh` 里做了 `obj-m += rwNova.o` + `rwNova-y := hwBreakpointProc_module.o`
   （源文件名不变，只改加载名与产物名）。**proc 节点名不受影响** —— 它来自
   `CONFIG_PROC_NODE_AUTH_KEY`（`dce3771681d4c7a143d5d06b7d32548e`），所以
   agentd 的探测路径照旧。

   **改这里最容易出事的一点**：原来 `build/ko/` 里的 afei prebuilt 靠「与自编译版
   **同名覆盖**」被消灭。改名后不再同名 → prebuilt 会以 `hwBreakpointProc_*.ko`
   留在 staging，而 `install_ko.sh` 会遍历目录里所有 `.ko` 逐个 insmod →
   **那份 prebuilt 一加载就可能打崩整机**（枚举从 0 起始、命中回调用 mutex）。
   `deploy.ps1` 现在显式 `Remove-Item $Stage\ko\hwBreakpointProc_*.ko`，别删这行。
   同理 push 要按 **staging 目录**遍历，不能按 `$koFiles`（rwNova 是后补进 staging 的）。

21. **hwbp 曾被误判为「不可用」**（2026-10-22，已改判）。当时把 `GET_NUM_BRPS=-1`
   直接写成「模块命令码不一致」，实际是 bp 节点错指到了内存模块。**以第 22/23/24 条为准**，
   别再照旧文案去重编模块。

22. **两个模块的 proc 节点长得一样，光看路径分不出谁是谁**（2026-10-22 修）。
   都是 `/proc/<auth_key>/<auth_key>`（mem `e84523…` / bp `dce377…`），
   把 `--rwproc-bp-key` 写成 mem 的 key 时，open 会**成功**，然后 GET_NUM_BRPS
   打到内存模块上返回 -1 —— 表现和"模块命令码不对"一模一样。
   现在 `driver.c` 的 `rwproc_open_node` 带类型校验：打开后先发一次
   `GET_NUM_BRPS`，`read>0` 判为断点模块、`read<=0` 判为内存模块，不匹配就
   关掉并明确报「is the memory module, not breakpoint — key 配反了」。
   **探测只有这一条命令是安全的**：内存模块的 cmd4 是 WRITE_PROCESS_MEMORY、
   cmd5 是 CLOSE_PROCESS，拿它们试探会在真正的断点上写内存 / put_pid。

   **自动扫描（不传 key）实测在真机上是无效的**：模块 `hide_procfs_dir` 之后
   `readdir("/proc")` 列不出 key 目录、`access()` 也失败 → 「memory module node
   not found」。所以 `start_agentd.sh` 里那两行显式 key 是**必需**的，别当成冗余删掉。
   扫描分支留着只为模块未隐藏的场景（本地调试）。

23. **hwbp 在 `[stack]` 最后一页（紧邻栈顶 4KB）注册会 `EINVAL`**（2026-10-22 实测）。
   同一 vma 里 0x…df000 成功、0x…ff000~0x…fff00 全部 `errno=22`，
   与进程无关（换 pid 一样），换 type/len 也一样。设断点时避开栈顶那一页。
   **排查时别被 `no thread registered` 带偏**：`start_all` 把每个线程的失败原因
   全吞了，地址不合法和进程不可注册报的是同一句话。用
   `build/ko/_dev/hwbp_inject.c` 逐步打印 OPEN/INST/UNINST 的返回值与 errno。
   另外 `HWBP_MAX_HANDLES=16`（模块静态句柄表，`hwBreakpointProc_module.c:28/288`）：
   已注册 16 个断点时再 INST 由模块直接回 `ENOSPC`。这是正常语义，但它同时意味着
   **watch 对一个进程最多只覆盖前 16 个线程**（`start_all` 按 `/proc/<pid>/task`
   顺序注册，第 17 个起全部 ENOSPC 被 `continue` 跳过）。systemui 这种 200+ 线程的
   进程，只有前 16 个线程的命中能被看到 —— 别把"没命中"当成"地址不对"。

24. **hwbp 命中链路已真机验证**（2026-10-22 夜，`hits=64` + 寄存器/反汇编都对）。
   验证用自建靶子而不是猜地址：`build/ko/_dev/bp_target.c`（每秒写 1000 次全局变量，
   打印 `&g_var` 和 `tick` 的地址，`-static` 非 PIE 所以地址稳定），
   `hwbp_hits.c` 直连模块看 COUNT/DETAIL。游戏进程会冻结、栈地址又挑不出稳定可写点，
   拿它们验证命中率纯靠运气。

   **踩到的真 bug（`driver.c` 的 `rwproc_hwbp_get_hits`）**：33 字节请求头里的
   `buf_size` 才是内核决定回写多少的字段，`resp_cap`（read() 的 count）不进协议头。
   原来 DETAIL 调用传的是 `NULL, 0, bytes`（data_size=0）→ 模块
   `if (size < sizeof(HWBP_HIT_ITEM)) return -EINVAL;` 恒失败 →
   **watch_poll 永远 0 条**，而 COUNT 正常（它不检查 size），所以表面上
   "断点能注册、就是没命中"。已改为 `NULL, bytes, bytes`,并加了结构护栏
   `tests/unit/check_hwbp_detail.py`（`run_tests.bat` 会自动跑，正反两向验过）。

   **能力边界**：模块 `hit_arr` 只在 `hit_count < HWBP_HIT_CAP(1024)` 时写入
   （`record_hit_details`），满了就不再记录 —— 高频断点看到的是**最早 1024 次**
   命中，游标推完就永远 0。watch 适合低频事件（某函数被调用、某变量被改几次），
   不适合监控高频写。

27. **KO 版本谱系 + 读路径的 memstart_addr 陷阱**（2026-10-23 两次实测定案）。

   **现役 KO**：`rwNova33_6.12.69.ko`（305824 = 内存读写）+ `rwNova_6.12.69.ko`
   （359504 = 硬件断点），都由 `scripts/build_rwproc_ko.sh` 从当前源码重编，
   ITEST ALL PASS、全量搜索 3.3s / 20698 条不崩。

   | 文件 | 大小 | 状态 |
   |---|---|---|
   | `rwNova33_6.12.69.ko` | 305824 | **现役**（含 memstart_addr 修复 + 隐藏项） |
   | `rwNova_6.12.69.ko` | 359504 | **现役**（hwbp，加 `ccflags-y += -Wno-error` 后可编） |
   | `rwproc_v2.ko` | 307640 | 历史最好的旧版，保留备查 |
   | `rwProcMem_gki.ko` | 307584 | 读全 0，弃用 |
   | `_dev/rwproc_dbg.ko` | 330712 | 唯一带 `CONFIG_DEBUG_PRINTK`，只用来抓页表日志 |
   | `prebuilt/rwProcMem_slim.ko` | 39552 | **vermagic 不匹配**，insmod 直接 Exec format error |

   ### 读路径的真实根因（推翻此前两次结论）

   现象：`driver self-test` 读**自身进程**时好时坏（"both reads FAILED, r=4 但内容 0"），
   部分页静默读不到；跨进程读低地址却正常。

   此前写过两个错误结论，都废弃：~~"virt_addr_valid 把合法 pfn 判成非法"~~、
   ~~"paddr 越界"~~（paddr 0x89b68124c 落在 0x880000000–0x8af8fdfff 段内，合法）。

   **真因**：`phy_mem_fast.h` 的守卫算 pfn 时用了
   `PHYS_PFN((unsigned long)cur - PAGE_OFFSET)` —— 少减了 `memstart_addr`。
   arm64 线性映射是 `__va(p) = p - memstart_addr + PAGE_OFFSET`，所以
   `cur - PAGE_OFFSET = p - memstart_addr`，**不是 p**。本机 memstart_addr ≈ 2GB
   （0x80000 页）。诊断版实测同一次读：

   ```
   rwdbg[0] pfn_calc=0xa6238e   ← PHYS_PFN(cur - PAGE_OFFSET)，少了 2GB
            pfn_v2p =0xae238e   ← virt_to_pfn(cur)，正确
   ```

   后果：真实 paddr 有效、但错位的 pfn 落进 MMIO 空洞时 `pfn_to_online_page` 返回
   NULL → 整页静默丢弃。**每页是否被丢取决于地址，所以表现为"时好时坏"，
   而不是稳定失败 —— 这是它查了两轮才揪出来的原因。**

   修法（已落地）：两处守卫改成 `pfn_to_online_page(virt_to_pfn((void *)cur))`，
   `virt_to_pfn` 内部走 `__virt_to_phys`，已扣 memstart_addr。

   **修复前后实测（同一台设备、同一进程类型）**：全量搜索 count 13681 → **20698
   （+51%）**，耗时 3.3s 基本不变 —— 也就是说**之前约三分之一的页根本没被读到**，
   搜索结果是"漏"的。这条对所有读写路径生效，不只是 self-test。

   **部署源**：`build/ko/rwNova33_6.12.69.ko` 已入库（`build/ko/*.ko` 是早期就被
   跟踪的少数文件）；`build/ko_built/` 未跟踪、只在本机。换机器先 `ls -la` 对大小。

28. **「一个驱动变成两个」的根因：deploy 的 staging 目录从不清理**（2026-10-23 修）。

   `deploy.ps1` 把 `build\ko\*.ko` 拷到 `%TEMP%\ggmcp_stage\ko\`，再**按 staging
   目录遍历推送**（这是为了不漏掉后补进去的 rwNova）。但 staging **跨次保留且从未
   清空** —— 2026-10-21 首次部署时 `build\ko\` 里那个 `apr_dlkm.ko`（rwProcMem 的
   原始编译名，`build_rwproc_ko.sh` 的产物就叫这个）被拷进去后再没出来，
   而 `build\ko\` 后来改用了 `rwProcMem_<内核版本>.ko` 命名，
   于是这份 10-21 的旧文件每次部署都被推到设备上。

   后果：设备上 `apr_dlkm.ko`（39552）与 `rwProcMem_6.12.69.ko` 并存。两个都是
   rwProcMem、模块名不同（`apr_dlkm` / `rwProcMem_module`）所以都能 insmod，
   但 proc 节点只有一个 —— 先加载的占住 `/proc/<key>/<key>`，后加载的
   `proc_mkdir` 失败 fallback 到 /proc 根（`strings apr_dlkm.ko` 里能看到这句日志）。
   **agentd 永远打开先加载那个**，于是「换了 KO 但行为没变」就成了灵异现象。

   已修：`deploy.ps1` 每次部署前 `Remove-Item "$Stage\ko\*"`。
   若再看到设备上出现 `apr_dlkm.ko`，说明 staging 又被污染了 —— 直接删掉即可
   （它没有任何独立功能，就是 rwProcMem 的别名）。

29. **隐蔽性基线（2026-10-23 落地，动驱动前先读这条）**。

   两个内核模块原本各 10 处裸 `printk`（含 `Hello/Goodbye <设备名>`、
   `[rwProcMem] module hidden by default`），dmesg 里 `grep -i rwproc` 能直接看到；
   proc 节点权限是 `S_IRUGO|S_IWUGO`（0666）。**任何 App 都能打开它**，等于
   "藏了个地址但没锁门"。现在：

   | 项 | 改法 |
   |---|---|
   | 20 处裸 `printk` | → `printk_debug`（受 `CONFIG_DEBUG_PRINTK`，当前关闭＝空函数） |
   | 4 处 `proc_create` 权限 | `S_IRUGO\|S_IWUGO` → `S_IRUSR\|S_IWUSR`（0600） |

   真机实测（重启后干净状态）：`dmesg` 里 rwproc/rwnova **0 条**；节点 `-rw-------`；
   非 root 打开 → `Permission denied`；`ls /proc` / `/proc/modules` / `/sys/module`
   均查不到。**改完必须同时验证这三条，缺一条就是没生效。**

   **仍是暴露面，按性价比排**（要动之前先确认目标）：
   - `CONFIG_PROC_NODE_AUTH_KEY` 是**编译期固定值**（内存 `e84523…` / 断点 `dce377…`），
     全设备同一把钥匙 —— .ko 一旦外流就永久失效。`strings` 能直接读出来。
   - 目录项只在 readdir 里被过滤（kprobe hook `proc_root_readdir`），
     **知道路径就能 open**。真正的"不存在"要靠 `anon_channel.h`（匿名 inode，不建
     proc 节点），代码已写好但 `ver_control.h` 里是 `//#define CONFIG_USE_ANON_CHANNEL`。
   - `cfi_bypass.h` 只 patch、**没有 restore**，内核 text 长期处于被改状态，
     完整性自检能抓到。
   - debugfs 挂上后 `/sys/kernel/debug/kprobes/list` 会列出 `proc_root_readdir` 探针。

   **hwbp 编不过的坑（已解）**：`anti_ptrace_detection.h` 里两个未引用的 static 函数
   被完整 GKI 树的 `-Werror` 判死，`KCFLAGS=-Wno-error` **压不住**（那是内核自带的），
   必须用模块级 `ccflags-y += -Wno-error`。另外 `hwbp_proc.c` **不参与编译**
   （Makefile 只有 `obj-m += hwBreakpointProc_module.o`），里面的 cdev/`/dev` 节点
   全是死代码 —— 别照着它找 `/dev/rwNova`。

31. **匿名 inode 通道已启用（2026-10-23）—— 但"节点不建"做不到，别被这个说法误导**。

   现状：`CONFIG_USE_ANON_CHANNEL` 打开后，agentd attach 时通过 proc 通道发
   `CMD_ANON_GET_FD(14)`，驱动在当前进程 `fd_install()` 一个
   `anon_inode:rwproc`，之后**内存模块的所有命令改走 `ioctl(fd, 33, buf)`**。
    实测：`ls -l /proc/<agentd>/fd` 里是 `6 -> anon_inode:rwproc`；
    搜索/读写/refine 全部正常（ioctl 路径与 read 路径耗时持平，0.11~0.18s）。

   **为什么不能"纯匿名、proc 节点完全不建"**（这条别再去试，会白做一轮）：
   `fd_install()` 装的是**发起该系统调用的进程**的 fd 表。它只能在用户态
   触发的路径里执行，而用户态第一次触发的唯一入口就是 proc 节点
   （`anon_channel_install_fd()` 的注释里有 2026-10-10 的真机验证：
   放 init 里只会装进 insmod 进程，insmod 一退 fd 就没了）。
   所以 proc 节点必须保留 —— 它是 bootstrap，也是 agentd 崩溃重启后的重连入口。

   **改动要点**（`agentd/driver.c`）：
   - `rwproc_call_ex` 内部判断 `use_anon = (rwproc_anon_fd >= 0 && fd == rwproc_mem_fd)`，
     命中就换 fd + 走 ioctl；**断点模块仍走 proc read**（只有内存模块启用了匿名），
     两个 fd 不能混用。
   - 取 fd 用 `rwproc_call`（out=NULL）而不是 `rwproc_call_ex`：驱动返回值就是
     fd 号，而 call_ex 的 out 长度校验会把 "r=fd号" 当成回写长度，fd 号大一点
     就误报 `result overflow`。
   - `ioctl` 的 cmd 必须 ≥ 33（驱动用它做布局校验），arg 指向 `[33B 头][payload]`。
   - 拿不到时只记日志、继续用 proc read —— 不致命。

   **仍然存在的暴露面**：proc 节点还在（0600 + readdir 隐藏），知道 key 就能开。
   想再进一步只能接受"agentd 不能重启"（删节点 = 断了重连入口），或者
   让驱动主动往指定 pid 塞 fd（`task_work_add`，但 pid 要在 insmod 时传）。

32. **fd 表才是最大的暴露面；两个模块现在都走匿名通道**（2026-10-23 定案）。

   ### fd 表比节点本身更容易被命中

   agentd 的 `/proc/<pid>/fd` 里，proc 节点的 fd 会显示成
   `N -> /proc/<key>/<key>` —— **扫一遍 fd 表就能发现节点，不需要知道 key**。
   之前几轮都在优化"怎么藏节点"，没看"谁还开着它"。

   现在两个模块都升级到匿名 inode：

   | 模块 | 通信 | fd 表显示 |
   |---|---|---|
   | rwNova33（内存） | ioctl | `anon_inode:dmabuf` |
   | rwNova（断点） | ioctl | `anon_inode:dmabuf` |

   拿不到匿名 fd 时才退回 proc read（`pmfd<0` 式兜底，不影响功能）。

   ### 三个改动点（按踩坑顺序）

   1. **匿名 inode 名字 `"rwproc"` → `"dmabuf"`**：原名等于在 fd 表里写着
      "内存读写通道"，dmabuf 是 Android 上极常见的内核对象。
   2. **拿到匿名 fd 后立刻 close 掉 proc fd**（`rwproc_anon_upgrade()`）：
      节点路径从 fd 表消失；`rwproc_mem_fd` 直接改指匿名 fd，所有
      `rwproc_mem_fd < 0` 的判断照旧成立，调用点不用改。
   3. **⭐ 按 fd 归属取匿名 fd**（这是 hwbp 的 bug，害我排查了两轮）：

      ```c
      // 错：不管哪个模块都取内存模块的 fd
      int anon_fd = use_anon ? d->rwproc_anon_fd : -1;
      // 对：
      if (d->rwproc_anon_fd >= 0 && fd == d->rwproc_anon_fd)      anon_fd = d->rwproc_anon_fd;
      else if (d->rwproc_bp_anon_fd >= 0 && fd == d->rwproc_bp_anon_fd) anon_fd = d->rwproc_bp_anon_fd;
      ```

      症状极具误导性：断点模块的命令被发到内存模块的匿名 fd 上，
      `GET_NUM_BRPS(3)` 被当成 `READ_PROCESS_MEMORY` 执行，返回 **-14 (EFAULT)**
      （空句柄），表现为 `ioctl(cmd=3) failed: Bad address`。
      而 `getCpuNumBrps()` 是纯位运算 `((read_cpuid(ID_AA64DFR0_EL1)>>12)&0xf)+1`，
      根本不可能返回负数 —— 这个矛盾就是定位的突破口。

   ### 排查工具（`build/ko/_dev/`，不入库）

   - `anon_probe.c`：单模块 —— 分别测 proc read / 取 anon fd / anon ioctl 三步。
   - `anon_two.c`：复现 agentd 的双模块 fd 序列，打印每个 anon fd 上 `cmd=3`
     的返回值，**一眼看出 fd 归属**（返回 6 = 断点模块，返回 -14 = 内存模块）。
   下次怀疑"fd 发错模块"，先跑 `anon_two`，别再靠日志猜。

   ### 仍然做不到的（架构限制，别硬试）

   - **proc 节点不能删**：`fd_install()` 装的是发起系统调用的进程的 fd 表，
     只能在用户态触发的路径里执行；而用户态第一次触发的唯一入口就是 proc 节点。
     节点同时是 agentd 崩溃重启的重连入口。
   - **key 不能随机化**：key 是路径名，agentd 靠它 open；而 readdir 又被
     `hide_procfs_dir` 过滤掉了，随机化后 agentd 无从发现（死锁）。
     要突破只能让驱动用 `task_work_add` 主动往指定 pid 塞 fd，pid 得在 insmod
     时传、agentd 重启即失效 —— 那是换一种运维约束，不是"修好"。

   ### 测 hwbp 的坑

   ARM64 只有 6 个硬件断点槽。`gg_watch_set` 之后**必须 `gg_watch_clear`**，
   否则后续 itest 的 `hwbp INST` 返回 -1（表现为"代码坏了"，实际是槽被占）。
   另：itest 的 hwbp 段在探测失败时会打 `[skip]` 然后继续，
   **`ITEST ALL PASS` 不等于 hwbp 验过** —— 必须看有没有 `bp id` 那两行。

33. **隐藏性自检脚本 `scripts/check_stealth.sh`**（2026-10-23）。

   改完驱动或 agentd 的通信层，**先跑这个再谈"改好了"**：

   ```
   adb push scripts/check_stealth.sh /data/local/tmp/cs.sh
   su -c 'cp /data/local/tmp/cs.sh /data/adb/GG-Ghost/check_stealth.sh
          chmod 755 /data/adb/GG-Ghost/check_stealth.sh
          sh /data/adb/GG-Ghost/check_stealth.sh'
   ```

   12 项检查，任一 FAIL 退出码 1：

   | # | 检查项 | 判据 |
   |---|---|---|
   | 1 | dmesg | 无 rwproc/rwnova/rwproc33 字样 |
   | 2 | /proc/modules | 查不到模块 |
   | 3 | /sys/module | 查不到模块 |
   | 4 | ls /proc | 查不到两个 key 目录 |
   | 5 | 节点权限 | `-rw-------` |
   | 6 | agentd fd 表 | 无 `/proc/<key>/<key>` 链接 |
   | 7 | 匿名 fd 数 | ≥2（两个模块都升级） |
   | 8 | fd 名 | 无 `anon_inode:rwproc`（应为 dmabuf） |
   | 9 | kallsyms | 无 `[rwNova` 符号 |
   | 10 | debugfs kprobe | 未挂载 或 无 proc_root_readdir 探针 |
   | 11 | 非 root 打开 | 必须被拒 |
   | 12 | 功能 | agentd 日志有 `channel OK`（隐藏不能以功能为代价） |

   第 12 项是刻意加的：**只验"看不见"不验"还能用"，很容易把驱动改瘸了还庆祝隐藏成功**
   （本轮 hwbp 的 fd 归属 bug 就是这样 —— 隐藏全过、功能全挂）。

   当前实测：**PASS=12 FAIL=0**。

34. **CFI 补丁无法消除（别再试编译期方案）**（2026-10-23 论证）。

   `cfi_bypass.h` 干的事：用 `aarch64_insn_write` 逐 4 字节把
   **`__cfi_check_fail` / `__cfi_slowpath` / `__ubsan_handle_cfi_check_fail_abort`**
   这几个失败处理函数改成空操作。它不碰检查逻辑本身，只让"检查失败"不 panic。

   为什么必须这么做：内核开 `CONFIG_CFI_CLANG=y` 后，**所有间接调用点**都插了
   `__cfi_check()`。hwbp 把 handler 注册成 perf_event 回调，内核在**自己的调用点**
   用函数指针调它 —— 这个检查在调用方，不在模块里。

   所以**编译期关掉模块的 KCFI 插桩（`-fno-sanitize=kcfi`）解决不了**：
   模块自己少插几个检查，内核侧的检查照旧，回调一进来就 panic。
   真要用编译期方案得连内核一起改（不现实）。

   现状风险：patch 常驻 → 内核 text 与磁盘 Image 不一致，理论上完整性自检能抓到。
   判据是"需要 root + 主动算 text hash"，比 dmesg/fd 表那类被动暴露面高得多。
   **做隐藏性优化时不要动这块** —— 关掉直接 panic，且没有替代路径。

   顺带：`disable_kprobe_blacklist()` 也在同一个 `#ifdef` 里，它是给
   `rw_sym_lookup` 探敏感符号用的（`modules` 链表等），关掉会让 UNHIDE 逃生舱失效。

43. **共享内存控制环（anon_ring）：审查已过，真机未验（2026-11-03）**。
   工作区那批未提交改动 = 内核 `anon_ring.h`（mmap 数据面，去 ioctl 足迹）
   + agentd `--ring` 对端 + 两模块 fd 改名 `[eventpoll]`。对抗性审查结论、
   修复与真机验收清单见 **`docs/engineering/RING_REVIEW_20261103.md`**，要点：

   - **已修的 P0**：环超时后不得复用（同一 seq 会让新命令静默拿到旧命令的
     结果）—— submit 超时/STOPPED 返回 -2，call_ex 拆环回落 ioctl，重连才重建。
   - **改环/动这段前先读那份报告的"遗留观察"**：R-4（handler 定长回写无视
     buf_size）是有意不修，理由在报告里。
   -    真机验收 5 步在第 5 步要**同步改 `check_stealth.sh` 第 8 项判据**
     （fd 名 dmabuf → [eventpoll]）。当前设备上的 .ko 没有 .mmap，`--ring`
     会自动回落 ioctl——这条本身就是一条回归验证路径，别当成故障排查。

