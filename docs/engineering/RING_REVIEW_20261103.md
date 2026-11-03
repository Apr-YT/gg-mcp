# anon_ring 控制环 —— 上真机前的对抗性审查（2026-11-03）

审查对象：工作区未提交的「共享内存控制环」整批改动（内核 `anon_ring.h` 333 行 +
`anon_channel.h`/`rwProcMem_module.c` 的环对接与 PF_KTHREAD 防护 + agentd
`driver.c` 的环对端 + `--ring` 开关）。目的：这批代码只过了交叉编译，
在烧真机时间之前先撕一遍并发 / 生命周期 / 协议边界。

## 结论

| # | 级别 | 发现 | 处置 |
|---|---|---|---|
| R-1 | P0 | 超时后环仍被复用 → 新命令静默拿到**旧命令的结果** | 已修（agentd） |
| R-2 | P1 | 内核生存期注释给出错误的正确性依据（锁），真实保序来自 vma 的 file 引用 | 已修（注释） |
| R-3 | P1 | 断点模块 fd 伪装名还是 `dmabuf`（内存模块同批已判死该名字） | 已修（改名） |
| R-4 | P2 | ioctl 路径部分 handler 无视 buf_size 定长回写（1037B 等），环路径反而被 RING_MIN_SIZE 挡住 | 不修，见「遗留观察」 |

## R-1：超时复用环 → 旧结果冒充新命令（已修）

`rwproc_ring_submit` 的 seq 以 `done_seq+1` 为基准。超时返回后环指针没动，
下一笔命令会算出**与超时请求相同的 seq**。若内核线程仍在处理那笔旧请求：

1. 用户态把新请求写进 slot，与旧请求的处理并发（payload 数据竞争）；
2. 旧请求完成 → `done_seq` 推进到该 seq → agentd 误认为**新命令已完成**，
   读走 `h->result` = 旧命令的回写字节与返回值。

新请求实际从未执行，调用方却拿到一次"成功"。这正是协议注释（anon_ring.h 第 6 条）
自己禁止的行为，但 agentd 侧没有实现对应逻辑。

修法（`agentd/driver.c`）：`rwproc_ring_submit` 超时/检测到 `state==STOPPED` 时
返回 `-2`；`rwproc_call_ex` 收到 -2 即 `munmap` 拆环（vm_close → kthread_stop
会阻塞到旧请求做完，有界）并记日志，此后所有命令回落 ioctl，直到下次重连
（`rwproc_try_anon` → `ring_setup` 重建）。附带收益：线程已停时不再白等 10s。

## R-2：生存期注释的错误依据（已修注释，代码未动）

原注释称 `stop_lock` 保证 close(fd) 与 munmap 并发时不 UAF。事实上**这两条路径
不会并发**：vma 持有 file 引用（`mmap_region` 里 `vma->vm_file = get_file(file)`），
环还映射着时 fd 的最后一个 file 引用就不归零，`release`（detach）必然晚于
`vm_close`，detach 正常只是兜底。

代码保留不动（防御纵深本身无害），但注释改为陈述真实不变量。**为什么值得较真**：
原注释把正确性归因于一把并不提供该保证的锁 —— 后续维护者若照注释类推，
在别的路径上单独依赖 stop_lock 就会翻车。

## R-3：断点模块伪装名不一致（已修）

内存模块本轮已把 fd 名从 `dmabuf` 改为 `[eventpoll]`，并写明理由：真 dma-buf fd
的 readlink 是设备路径，`anon_inode:dmabuf` 本身不在合法形态集合里，名字比对一扫
就露馅。断点模块的 `anon_channel.h` 是独立副本，仍挂着 `dmabuf`。已同步改为
`[eventpoll]`（两个 epoll fd 对任何 App 都是常态）。仍只骗名字比对，行为探测
（EPOLL_CTL 探测）会暴露 —— 上限同内存模块注释。

## 验证状态

**本机已验证**（2026-11-03，改动后重跑）：

- `tests/unit/run_tests.bat`：15 个单测 + 6 道结构护栏全绿（含 ko 快照同步 185 文件一致）
- `scripts/build_ndk.bat`：agentd 全绿，`-Wall` 零警告

**未验证**（设备不在位，按第 3 条债务纪律不得声称已验证）：

- 环的全部运行时行为。内核侧改动（注释 + 断点模块改名）连同整批 ring 功能都未上机。
- R-1 修复的超时路径**没有可用的本地复现场景**（需要真机驱动线程慢请求/停转）。

**真机验收清单**（设备接上后，按序）：

1. `scripts/deploy.bat` 部署新 KO（注意：需重编 `CONFIG_ANON_RING` 版本的
   `rwNova33`，现有设备上的 .ko 没有 .mmap，agentd 会走 fallback —— 这本身
   也是一条回归验证：无环驱动 + `--ring` 必须无损回落 ioctl）
2. `run_itest.ps1` ALL PASS（不带 --ring，验证默认路径零回归）
3. `RING=1` 重启 agentd，日志应出现 `ring ready`；重跑 itest
4. 针对性验证 R-1：跑一次长搜索观察是否超时；人为 kill 驱动线程场景可跳过
   （不可构造），但"超时后命令仍可用（走 ioctl）"必须在第 3 步顺带确认
5. `scripts/check_stealth.sh` 12 项全过（fd 名现为 `[eventpoll]`，第 8 项判据
   需同步更新：查 `anon_inode:rwproc` 改为查 `anon_inode:dmabuf` 不存在）

## 遗留观察（有意不修）

- **R-4**：`OnCmdInitDeviceInfo` 等 handler 定长回写（1037B/8B/16B）不看
  `buf_size`。ioctl 路径上调用方传小 buf 就可能越界写用户内存（copy_to_user
  落在未映射页才报错，落在已映射页 = 静默破坏调用方堆）。环路径反而安全
  （payload ≥ PAGE_SIZE-33）。agentd 现有调用全部传够尺寸，故不修；动这块
  要重验所有命令，收益不成比例。若未来有第三方调用方，需在 ioctl 入口加
  与环相同的 clamp。
- 轮询线程 `rwnringd` 空闲占 CPU（200~400us usleep 循环）：设计取舍，
  见 anon_ring.h 文件头第 2 条，非缺陷。
- `watch.c:340` snprintf 截断警告：安全截断，误报性质，不动。
