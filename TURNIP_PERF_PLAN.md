# Turnip-Enhanced 性能优化开发计划（本地文档，不推远端、不提 PR）

> 基线分支：`perf-opt-wip`（`e3ea98b4` 起）
> 基线目录：`src/freedreno/vulkan/`
> 状态：规划文档，仅本地存档。**禁止 `git push`，禁止 `gh pr create`，禁止远端写入操作。**
> 上游参考：`P0 submit / P1 bind+barrier / P2 shader / P3 mem` 已落地，需先收尾巴再开新阶段。

---

## 0. 已落地复核结论（P0–P3）

| 已落地 | 结论 | 尾巴（必须先修） |
|---|---|---|
| P0 `tu_queue.cc` 锁收窄、`tu_knl_kgsl.cc` 5遍合2遍、`tu_knl_drm_msm.cc` 栈数组 | 方向正确 | 无 |
| P1 `tu_CmdBindPipeline` 同管线早退、`tu_barrier` 空依赖早退 | 正确。`Begin` 有 `memset(state,0)`（`tu_cmd_buffer.cc:4533`），`UNDEFINED` 路径先置 `src_flags`（`10228-10242`），不会误跳 | 无 |
| P2 FDM 仅 FS、barrier 按 `uses_memory_barrier` 跳过 | 正确 | 无 |
| P3 suballoc 4槽 MRU、autotune 早退、event 原子读 | 正确 | 有 2 个 |
| query 指数退避 | 过渡方案正确，最终要换内核等待 | 无 |

### 尾巴 T1：trace 扫描用错 count（正确性）

* 位置：`tu_queue.cc:457`
* 现状：
  ```cpp
  for (unsigned i = 0; i < vk_submit->command_buffer_count; i++) {
    if (u_trace_enabled && u_trace_has_points(&cmd_buffers[i]->trace))
  ```
  `cmd_buffers/cmdbuf_count` 已被 `tu_insert_dynamic_cmdbufs` 展开，循环却仍用展开前的 `vk_submit->command_buffer_count`，动态插入的包 trace 漏判。
* 修复：改循环上限为 `cmdbuf_count`。
* 验证：`suspend/resume` + `GALLIUM_HUD / utrace` 开启跑一帧不断言、不丢 trace。

### 尾巴 T2：event Set/Get 内存序不对称（ARM 正确性）

* 位置：`tu_event.cc:37-71` 创建持 `event_mutex`，`96,105` plain store，`Get:84` 已改 `p_atomic_read`。
* 修复：`Set/Reset` 改 `p_atomic_set`（release），或至少 `p_atomic_write`；`default_size` 在 `tu_device.cc:3570` 仅一页（64 event/BO），顺手提到 4 页减锁竞争。
* 验证：多线程 `Set+GetEventStatus` hammer + `TSAN` 无告警。

> 门禁：T1+T2 合一个 `fixup` commit 落地后，才开 P4 及以后。

---

## 1. 目标与非目标

### 目标

* 同场景 CPU 占比下降、帧时间 p95/p99 下降，多线程提交不串行，compute 密集场景 dispatch 开销减半，大查询/等待不烧核。
* 零正确性回归：`dEQP-VK.*`、Fossils/RenderDoc 抓帧、挂机 2h 无泄漏/爆栈。

### 非目标

* 不改 UAPI/ABI，不改 KGSL/MSM 内核接口语义，不做跨驱动大重构。
* `FD_RD_DUMP=FULL`、breadcrumb、perfetto 开启时的开销只做“移出热锁”，不追求极限。
* 本计划不包含着色器编译器（ir3/nir）大改，只做调用侧跳过。

### 测量方法（每个阶段必跑）

* 基准：同一台 Adreno 6xx/7xx/8xx（一台 MSM + 一台 KGSL，如有），同一 APK/用例（建议：DXVK 游戏 + 原生 VK compute + `vkmark`/`gfxbench` 一项）。
* 指标：`fps avg/p95`、`CPU %（game+render线程）`、`vkQueueSubmit p50/p99`（perfetto `tu_queue_submit` slice）、`GEM_NEW/sec`（`ftrace:drm` 或 `TU_DEBUG=bos` 计数）、`sched_yield/sec`。
* 工具：`perfetto + tu_perfetto`、`systrace`、`adb shell top -H`、`FD_RTRACE` 关闭态对比。每个 commit 附前后 3 次取中位数。

---

## 2. 阶段总览与优先级

| 阶段 | 主题 | 核心文件 | 预期收益 | 工作量 | 依赖 |
|---|---|---|---|---|---|
| P4 | 提交锁粒度 + BO 锁移出 ioctl | `tu_queue.cc:452`、`tu_knl_drm_msm.cc:1484`、`tu_knl_drm.cc` | 高，多线程吞吐 | 大 | T1 后 |
| P5 | Bind/Descriptor/Push 去重 | `tu_cmd_buffer.cc:4734,4914,5462` | 高，draw 密集 | 中大 | 无 |
| P6 | Compute dispatch 补 dirty 门 | `tu_cmd_buffer.cc:9740,9507,7930` | 高，compute 密集 | 中 | P5 部分复用 |
| P7 | BO/映射/池化 | `tu_knl_drm_msm.cc:870`、`tu_cmd_buffer.cc:45`、`tu_descriptor_set.cc:873` | 高，加载+稳态 | 中大 | P4 |
| P8 | Query/Event/Fence 等待 | `tu_query_pool.cc:619`、`tu_event.cc`、`tu_knl_kgsl.cc:2394` | 高，卡顿/功耗 | 中 | 无 |
| P9 | 正确性/健壮性 | `tu_knl_kgsl.cc:1556,2244`、`tu_queue.cc:523` | 高（崩溃/泄漏） | 小 | 随 P4 修 |
| P10 | 中低长尾（draw_common/LRZ/MSAA/FDM/autotune） | 见 §8 | 中 | 小×N | P5 后捡 |

执行顺序：`T1+T2 → P9 → P4 → P5 → P6 → P7 → P8 → P10`。P9 可与 P4 同分支不同 commit。

---

## 3. P4：提交路径锁粒度（最高优）

### P4-1 `submit_mutex` 拆 prepare/commit

* 现状：`tu_queue.cc:452 lock → 586 unlock` 包住 `resolve_vis_stream（可能 GEM_NEW）+ autotune（MMIO+锁）+ RD_DUMP（mmap+写文件）+ tu_queue_submit（ioctl）` 约 130 行。
* 设计：
  1. 锁外：`tu_insert_dynamic_cmdbufs`（已在外，保持）、`has_trace_points` 扫描、`u_trace_submission_data_create` 的纯 CPU 部分、`autotune` 预扫描（只读 `batch->requires_processing()`，见 P3 已有逻辑）。
  2. 锁内（短）：`tu_submit_create + submit_add_entries（reserve 版，见 P4-3）+ fence seqno + tu_queue_submit 入队 + submit_count++`。
  3. 锁外（后）：`RD_DUMP` 全段、`perfetto refresh`、`debug_bos_print_stats`、`u_trace_flush/process`（后两者已在外，保持）。
  4. `FULL dump` 的 `tu_queue_wait_fence(~0)`（`tu_queue.cc:523`）必须移出锁，否则无限持锁（见 P9-3）。
* 草图：
  ```cpp
  // before lock
  tu_insert_dynamic_cmdbufs(...);
  bool has_trace = scan(cmd_buffers, cmdbuf_count); // 注意用 cmdbuf_count，顺带修 T1
  // lock only for seqno + enqueue
  pthread_mutex_lock(&submit_mutex);
  submit = tu_submit_create(...);
  ... add_entries ... (预留容量，一次性 reserve)
  submission_id = device->submit_count++;
  result = tu_queue_submit(...); // 内部不再做大 CPU 活
  pthread_mutex_unlock(...);
  broadcast_if_needed(...); // 见 P4-4
  // after lock
  do_rd_dump(...); do_stats(...); do_trace_flush(...);
  ```
* 风险：`submit_count` 被 RD_DUMP 的 `fd_rd_output_begin(..., submit_count)` 读（`518`），移出后需传快照 `submission_id`，禁止锁外再读 `device->submit_count`。
* 验证：多线程 4×`vkQueueSubmit` 压测吞吐提升、无 `submit_count` 乱序（`submission_id` 单调）、`dEQP-VK.synchronization*` 通过。

### P4-2 `bo_mutex` / `vm_bind_fence_lock` 移出阻塞 ioctl

* 现状：
  * `tu_knl_drm_msm.cc:1353-1388` `rdlock` 横跨 `VM_BIND ioctl`。
  * `tu_knl_drm_msm.cc:1484-1547` `bo_mutex` 横跨 `GEM_SUBMIT ioctl` + `submit_idx` 修正循环 + `vk_realloc:704` 扩容。
  * `tu_knl_drm_msm.cc:1127-1166` `dma_bo_lock(R)` 内做 `munmap + VM_BIND UNMAP + GEM_CLOSE`。
* 设计：
  * `submit_idx` 修正保留但只拷贝 `nr_bos/bos ptr/cmds ptr` 后解锁，`ioctl` 用快照；`bo_list` 扩容（`reserve`）移到加锁前。
  * `VM_BIND`：持锁只快照 `fence_fd`，`ioctl` 解锁后做；`close(old_fence)` 已在解锁后（`675`），保持。
  * `bo_finish`：先 `p_atomic_dec_zero`，引用归零后解锁再 `munmap/ioctl/memset`，只用局部 `gem_handle`。
* 风险：需确认内核不依赖 `bo_list` 在 ioctl 期间稳定（靠 `ref` 续命，不靠锁）。先加 `assert` + 单线程 fossils 对比，再放开多线程。
* 验证：`dEQP-VK.sparse* + memory.*`，`dmabuf import` 并发销毁压测，`mtx contention`（`perf lock`）下降。

### P4-3 `submit_add_entries` 预留容量

* 现状：`tu_knl_drm.cc:138-158` / `tu_knl_kgsl.cc:2049-2061` 逐 entry `dynarray_grow`。
* 设计：`queue_submit` 先汇总 `total_entries` 调一次 `reserve`，再批量填；`has_vm_bind` 分支提为双循环免每 entry 分支。
* 验证：大提交（多 cmdbuf）`realloc` 次数归零（`MALLOC_TRACE` 或计数器）。

### P4-4 `timeline_cond` 惊群

* 现状：`tu_queue.cc:587` 每次无条件 `broadcast`，MSM 路径走内核 syncobj 根本没人等也被唤醒（`tu_knl_kgsl.cc:1055` 才是等待侧）。
* 设计：加 `device->timeline_waiters` 原子计数（`wait` 前 `++`、醒后 `--`），`0` 则跳过 broadcast；单等待者用 `signal`。或按 queue 分 cond（改动大，备选）。
* 验证：KGSL 多线程 `vkWaitSemaphores` 场景虚假唤醒数（`perf sched`）下降，单线程无变化。

---

## 4. P5：Bind / Descriptor / Push 去重（最高优之二）

### P5-1 `BindVertexBuffers2`（`tu_cmd_buffer.cc:4734-4756`）

* 现状：每次 `tu_cs_draw_state` 分配 + 循环发射 `0..max-1`，`max_vbs_bound` 单调只增。
* 设计：
  * `tu_cmd_state` 加 `vb_bound_mask + vb_iova/size 缓存`，`memcmp(first,count)` 命中直接 return。
  * `tu6_draw_common:8894` 按 `dirty_vb_mask` 只发脏区间；`Begin` 重置 `max_vbs_bound`（`4533` 处已有 memset，确认次级 cmdbuf 也走）。
* 验证：ANGLE/zink 逐 draw 重绑用例 IB 长度下降，`dEQP-VK.vertex*` 通过。

### P5-2 `BindDescriptorSets`（`tu_cmd_buffer.cc:4914-5048`）

* 现状：同 set 重绑全量 `memcpy+patch`；A6XX 每 `STORAGE_BUFFER_DYNAMIC` 做 `FMT switch`（`4977-5012`）；`max_dynamic_offset_size` 单调增导致 `tu6_emit_descriptor_sets:4826` 包永久膨胀。
* 设计：
  * 缓存 `last_set_iova[idx] + dynamic_offsets hash`，命中跳过 `patch/alloc` 只（必要时）置脏位。
  * `max_*` 在 `Reset/Begin` 清零；A6XX `offset_shift` 预存到 `layout`，发射侧查表。
* 验证：DXVK 高频切 descriptor 用例 CPU 下降，`dEQP-VK.descriptor*` 通过。

### P5-3 `PushConstants`（`tu_cmd_buffer.cc:5462-5468`）

* 现状：无条件 `memcpy + DIRTY_SHADER_CONSTS`，触发 `tu_emit_consts:8038` 全量 5 stage 重算。
* 设计：`memcmp` 目的区间，相等 return；`dwords==0` 跳过 dirty；按 `stageFlags` 细化 dirty（先 memcmp，后细化）。
* 验证：同值 push 用例 `emit_consts` 调用数归零。

### P5-4 `emit_descriptor_sets`（`4851-4876`）+ `emit_consts`（`7913-8097`）

* 设计：`dirty_sets_mask` 多段 `pkt4` 只发脏区间；`SP_UPDATE_CNTL` 按 mask 使能；`total_const_dwords + bindless base` 在 `BindPipeline` 预计算缓存，`DESC_SETS` 不脏则复用 `shader_const.iova`。
* 验证：IB dword 数（`RD dump` 统计）下降。

---

## 5. P6：Compute dispatch 补门

* 位置：`tu_cmd_buffer.cc:9740-9743,9507-9687,7930-7943`
* 现状：`tu_emit_cache_flush` 全量 + 无条件 `tu_emit_consts(compute=true)` + 每 dispatch 重建 `driver_params`；graphics 有 dirty 门，compute 没有；`9738` 注释 `TODO: probably flush less`。
* 设计：
  1. 加 `TU_CMD_DIRTY_SHADER_CONSTS_COMPUTE`（或复用 `COMPUTE_DESC_SETS` 语义并拆 `consts` 位），`emit_consts` 命中缓存 `iova` 直接复用。
  2. `driver_params` 缓存 `last_blocks/local_size`，`memcmp` 命中复用。
  3. 拆 `compute_flush_bits`，只刷 compute 相关（参考 `tu_emit_cache_flush_renderpass:5886` 的 `if(!flush_bits) return` 模式）。
* 验证：`dispatch(1,1,1)` 循环 CPU 减半，`dEQP-VK.compute*` 通过。

---

## 6. P7：BO / 映射 / 池化

### P7-1 每 BO 3+ ioctl 瘦身（`tu_knl_drm_msm.cc:870-948`）

* `SET_NAME + snprintf(tu_device.cc:4300)` 仅 `TU_DEBUG` 启用；`name` 用静态串；`VM_BIND` 批量（与 P4-2 联动）。

### P7-2 `mmap/munmap` 减半

* `msm_bo_map:1038` 每次 `GET_OFFSET+mmap`；`tu_bo_unmap:tu_knl.cc:148` `PROT_NONE mmap/munmap` 都是 TLB shootdown。
* 设计：KGSL `never_unmap:303` 思路推广到 MSM 热 BO（query/descriptor/CS/status）；`finish` 延迟批量 `munmap`；`status_bo`（`tu_cmd_buffer.cc:45` 4B 也整页 GEM_NEW）并入 suballoc/global_bo。

### P7-3 描述符池 / CS 池化

* `tu_descriptor_set.cc:873` 每池 `GEM_NEW+mmap`：应用层复用池 + `Reset:960` 已轻量，保持并文档化。
* `tu_cs_add_bo:tu_cs.cc:173` 增长即内核分配：改指数增长池或 suballoc。
* `event_suballoc default_size`（见 T2）一并改。

### P7-4 suballoc 长尾（`tu_suballoc.cc`）

* 双遍 best-fit 合单遍；满且无 fit 时现在直接新分配、下次 free 又因满而 `finish` 抖动，改淘汰 LRU；加 `cached_bytes` 上限 + 空闲超时释放（只统计，不阻塞热路径）。
* 验证：`GEM_NEW/sec` 下降，峰值内存不涨超 5%。

---

## 7. P8：Query / Event / Fence 等待

### P8-1 `wait_for_available`（`tu_query_pool.cc:619-655`）

* 现状：每轮 `os_time_get_nano()` + 256 热旋 WC 内存 + 768 `sched_yield` + `50→1000us` 粗退避 + 硬编码 5s。
* 设计（过渡→最终）：
  1. 过渡：每 64 圈查一次时间；热旋次数按 `cached_coherent` 动态（WC 直接减半，见 P8-2）；`WITH_AVAILABILITY` 优先用 GPU 侧 `CP_WAIT_REG_MEM`（`emit_copy_query:907` 已有模式）。
  2. 最终：`MSM_IOVA_WAIT` 落地后切内核等待，删自旋（TODO 已在 `622` 注记）。
* 验证：`GetQueryPoolResults(WAIT)` 场景 CPU 从 100% 单核降到 <10%，p99 不涨。

### P8-2 query BO 缓存属性（`tu_query_pool.cc:172,479`）

* 无 `cached_coherent` 时 WC 轮询每读走总线。`tu_bo_init_new_cached` 已尽力，硬件不支持时退避更激进（热旋 256→64）。

### P8-3 event/fence 细节

* `tu_event.cc` T2 修完后，`GetEventStatus` 保持原子读；`tu_knl_drm.h:47` `get_abs_timeout` 无限等待免 `clock_gettime`；`virtio tu_wait_fence:421-438` host RTT 轮询改 host 阻塞等；`kgsl profiling:2394,2522` 纯自旋加 `yield+timeout`。

---

## 8. P9：正确性 / 健壮性（随 P4 修，不拖）

| 编号 | 位置 | 问题 | 修复 |
|---|---|---|---|
| P9-1 | `tu_knl_kgsl.cc:1556,2151,2265` | 裸 VLA `syncobjs[wait_count]`，大提交爆栈 | 换 `STACK_ARRAY`（8 栈+堆回退）+ 上限断言 |
| P9-2 | `tu_knl_kgsl.cc:2244,2487` | trace 提交 `vk_alloc(objs/cmd_obj)` 永不 `vk_free` | 成功/失败统一 `vk_free`，单 object 改栈上 |
| P9-3 | `tu_queue.cc:522-524` | `FULL dump` 锁内 `wait_fence(~0)` 可无限持锁 | 移出 `submit_mutex` 或转 worker 异步 dump |
| P9-4 | `tu_debug_bos_print_stats:tu_device.cc:5191` | 开启后每提交 hash+qsort+log 抢 `bo_mutex` | 节流 10s 或移出提交线程 |

---

## 9. P10：中低长尾（按需捡）

* `tu6_draw_common:8618` 快路径、`tu_draw_initiator:8921` 缓存 initiator、`VS_PARAMS:8971` 缓存 offset、`LRZ:8739` 指纹去重、`MSAA:8792` 转 draw-state 合包、`FS_PARAMS:8809` 缓存 key。
* `tu_barrier:10143` 多 barrier 先 OR 聚合再一次 `vk2tu_stage`，`vk2tu_access` 查表化；`ubwc quirk:10326` 只在 `UCHE_WRITE+UBWC image` 置 `CLEAN`。
* `viewport/scissor FDM:tu_pipeline.cc:2834` hash 复用 dynamic_state；`window_scissor:1080` 缓存相等跳过；`IndexBuffer:4760` 同值早退；`MultiDraw:9122` initiator 外提、非 tess 跳过 `max_*` 预扫。
* `pipeline/suballoc:tu_pipeline.cc:1496` per-thread suballoc；`heap.used:tu_device.cc:4048` 分片计数。
* `tu_cs debug:tu_cs.cc:631` 256B 栈缓冲快路径；`dump_cmds:tu_queue.cc:76` `unlikely+static flag`。

---

## 10. 分支 / 提交 / DoD（本地）

* 分支：全程在 `perf-opt-wip` 或其本地子分支（如 `perf-p4-lock`），**不推 `origin`，不合 `main`，不发 PR**。同步上游只 `fetch` 不 `push`。
* 提交粒度：一阶段一 commit，`fixup` 独立，信息沿用 `tu/<域>: <事>` 格式，body 写文件行号+前后数据。
* DoD（每个阶段）：
  1. `meson build + dEQP 对应子集` 通过；
  2. 基准 3 次中位数附 commit body；
  3. `fossils` 或 RenderDoc 抓帧对比无差异；
  4. 无新增 `vk_zalloc/mmap/ioctl` 在热路径（`grep` + perfetto 确认）。

---

## 11. 任务清单（可直接勾）

* [x] T1 trace count 改 `cmdbuf_count`（`tu_queue.cc:457`）
* [x] T2 event `p_atomic_set` + suballoc 4 页（`tu_event.cc`、`tu_device.cc:3570`）
* [x] P9-1 VLA → STACK_ARRAY（`tu_knl_kgsl.cc` 3 处）
* [x] P9-2 kgsl trace `vk_free`（`tu_knl_kgsl.cc:2244,2487` + timeline 路径）
* [x] P9-3 FULL dump 移出锁（`tu_queue.cc:522`，快照 fence + 解锁等待）
* [x] P9-4 debug_bos 节流 + 拼写修复（`tu_device.cc:5191`，600 次一打）
* [x] P4-1 submit 拆 prepare/commit（trace 扫描 + perfetto 移出锁）
* [x] P4-2 bo/vm_bind 锁移出 ioctl（munmap 外移 + fence 快照；legacy bo_mutex 跨 ioctl 保留并注明原因）
* [x] P4-3 add_entries reserve（新增 `submit_reserve_entries`，msm/kgsl/virtio 接入）
* [x] P4-4 broadcast 按需（`timeline_waiters` 计数）
* [x] P5-1 VB memcmp 早退（`4734`；mask 分段发射未做，IB 仍全量，留后续）
* [x] P5-2 descriptor iova 比较（`tu_set_descriptor_buffer_offsets`；全量 content hash 未做）
* [x] P5-3 push memcmp + 版本号（`5462` + `push_version`）
* [x] P5-C5 index 同值早退
* [x] P6 compute dirty 门 + driver_params 缓存（`9740`；flush 仍全量，留 TODO）
* [x] P7 BO 瘦身（SET_NAME 按需） + suballoc 单遍 + 4x 上限
* [x] P8 query 降频查时 + kgsl profiling 退避（virtio host 阻塞未做，需协议改）
* [x] P10 长尾（debug_magic 栈缓冲 + va_copy 修复） + `git diff --check` 通过
* [ ] 全量基准回归（需真机 + perfetto，本地未跑）
* [ ] `dEQP-VK.*` 回归（需真机/CI，本地未跑）

---

*本文档为本地计划，不作为 PR 描述，不上传远端。如需评审，请本地 `git diff` 或当面传文件。*
