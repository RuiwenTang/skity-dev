# Vulkan Device Lost 恢复：Skity 侧修复与上层（AnimaX）适配建议

依据《AnimaX Vulkan 实验崩溃排查（release/4.4）》中 P0 两条 device lost 问题的修复。
本文前半部分说明 Skity 侧已落地的行为契约变化，后半部分是**上层（animax
`src/render/vk/surface_vk.cc` 等）必须跟进的修改建议**，附判定表与验证方案。

代码基线：skity main（本仓库）；animax release/4.4（排查文档口径）。

依赖的 skity 提交（两个都要在）：

1. `fix(gpu): stop using Vulkan handles after device loss`（#590，已合入）：
   lost 后所有入口停止触碰 Vulkan 句柄；
2. `feat(gpu): report device loss with a dedicated presenter status`：
   新增 `GPUPresenterStatus::kDeviceLost` / `SKITY_ERROR_DEVICE_LOST`，
   lost 不再混在 kError 里。

## 一、Skity 侧已修复内容（本仓库）

修复原则：device lost 之后**任何路径不再触碰失效的 Vulkan 句柄**，并把
「不可恢复」这一事实通过返回值上抛，由上层决定重建粒度。

| 位置 | 修改 | 对上层可见的行为 |
|---|---|---|
| `GPUCommandBufferVK::Init()` | `IsDeviceLost()` 早退 | `GPUDevice::CreateCommandBuffer()` 返回 `nullptr`（帧被丢弃） |
| `GPUCommandBufferVK::Submit()` | `IsDeviceLost()` 早退 | `Submit()` 返回 `false`，不提交 |
| `GPUNativeWindowVK::RecreatePresenter()` | lost 时拒绝重建 | `Resize()` / presenter 重建返回 `false` |
| `GPUPresenterVK::AcquireNextSurface()` | lost 时返回 `kDeviceLost` | **不再死等** `vkWaitForFences(UINT64_MAX)`；acquire 侧三处 fence 调用（等待/复位/image fence）检测到 `VK_ERROR_DEVICE_LOST` 也会 `MarkDeviceLost` 并返回 `kDeviceLost` |
| `GPUPresenterVK::Present()` | lost 时不调用 `vkQueuePresentKHR` | 返回 `kDeviceLost`（含 `vkQueuePresentKHR` 自身返回 DEVICE_LOST 的路径），presenter 置 broken |
| `HWCanvas::OnFlush` / `TextureManager` / `AtlasTexture` | `CreateCommandBuffer()` 判空 | lost 后丢帧并打日志，不再空指针崩溃 |

状态码语义（C++ `GPUPresenterStatus` / C `skity_result`）：

| 状态 | 含义 | embedder 动作 |
|---|---|---|
| `kSuccess` / `SKITY_SUCCESS` | 正常 | — |
| `kNeedRecreate` / `SKITY_ERROR_NEED_RECREATE` | swapchain 失效 | 重建 presenter（限次） |
| `kDeviceLost` / `SKITY_ERROR_DEVICE_LOST` | 逻辑设备丢失 | **重建整个 GPUContext**（唯一升级信号） |
| `kError` / 其他错误码 | 瞬态错误（尺寸为 0、OOM）或调用方契约违反 | **保守不动作**：跳帧 + 日志/计数，不触发任何重建 |

关键点：**lost 之后 presenter 级重建已经无效**（`RecreatePresenter` 会拒绝），
唯一恢复路径是销毁并重建整个 `GPUContext`（`VulkanContextState::Initialize`
会重建 logical device 并复位 lost 标志，这是设计内行为）。

单测：`test/ut/gpu/vk/gpu_context_vk_test.cc` 的
`VulkanDeviceLostTest.GuardEntryPointsAfterDeviceLost` 覆盖 Init/Submit 早退。

## 二、Animax 侧必须跟进的修改

### A. `surface_vk.cc:41` Present 返回值不能再丢弃（当前 P0 配套）

现状：`presenter->Present(std::move(frame_surface_))` 返回值直接丢弃，
surface 已被 move 走并析构。丢失的 acquired image 不会回到 BufferQueue，
free slot 持续减少，最终 `dequeueBuffer` 失败、驱动在 `QueuePresentKHR`
内部崩溃（`gpu_native_window_vk.cc` 注释描述的后果）。

建议处理矩阵：

| Present 返回 | 含义 | 动作 |
|---|---|---|
| `kSuccess` | 正常 | 维持现状 |
| `kNeedRecreate` | swapchain 失效（OUT_OF_DATE / SUBOPTIMAL / SURFACE_LOST） | 丢弃当前 surface，重建 presenter 后继续（限次） |
| `kDeviceLost` | 逻辑设备丢失（Skity 侧已置 broken 并标记 lost） | **直接进入 context 级重建**，不要先试 presenter 重建（必然失败） |
| `kError` | 瞬态/未知错误（尺寸为 0、OOM、调用方契约违反等） | **不动作**：跳帧 + 日志计数；连续超限再考虑降级，绝不因 kError 重建 context |

注意：`kDeviceLost` / `kError` 时 surface 已析构、image 已丢失，这是无法避免的
（swapchain 已死，归还也无意义）；重点是按上表选择恢复粒度，**不要把
kError 当成重建信号**——尺寸为 0（旋转/前后台切换瞬间）和瞬态 OOM 在
Android 上高频，见 kError 就重建会造成重建风暴。

### B. 重建分级与上限

`kNeedRecreate` 现在专指 swapchain 级（presenter 重建可恢复），
`kDeviceLost` 专指 context 级（presenter 重建必然失败，Skity 侧
`RecreatePresenter` 会直接拒绝），两层信号已经分开：

- presenter 级重建（`kNeedRecreate`）：保持现有路径，设置重试上限
  （建议 2~3 次），超过上限降级处理；
- context 级重建（`kDeviceLost`）：销毁 `GPUContext`、`GPUContextVK`
  及全部 GPU 资源（纹理、atlas、pipeline 缓存全部随之失效，需要清空
  缓存），用同一 `GPUContextInfoVK` 重新创建后再重建 presenter；
- context 级重建仍失败（极端情况，如驱动持续 lost）：降级到 GL 后端或
  停止渲染，避免无限循环。同样设置重建上限与退避。

### C. 判定信号（已简化）

拿到 `kDeviceLost`（C API 为 `SKITY_ERROR_DEVICE_LOST`）就是唯一需要
context 级重建的信号，直接执行，无需启发式组合。

补充兜底（旧版本 skity 只有 #590、没有新状态码时的兼容判定）：

1. `Present` 返回 `kError` 后 presenter 重建失败；
2. 连续 N 帧 `CreateCommandBuffer` 失败 / canvas Flush 丢帧日志
   （`HWCanvas::OnFlush: failed to create GPU command buffer`）。

> 可选增强（需要 Skity 公共 API 配合，暂未做）：在 `GPUContextVK` 暴露
> `IsDeviceLost()` 查询接口，上层可直接轮询。

### D. 帧丢弃路径的收尾

`cmd->Submit(...)` 失败（animax 侧 `hw_canvas` 调用点对应
`HWCanvas::OnFlush` 内部）或 `CreateCommandBuffer` 失败时：

- **不要调用 `Present`**（没有可呈现的内容，且 fence 未 signal）；
- Skity 侧已保证下次 `AcquireNextSurface` 不会死等（lost 早退），
  非 lost 的偶发失败（如 `vkEndCommandBuffer` 失败）仍依赖 presenter
  的 broken 机制，上层拿到空 surface / `kNeedRecreate` 时按 A/B 处理；
- 打点：记录丢帧原因，与 Slardar 现有 issue 分类对齐。

### E. 监控建议

- `kDeviceLost` / `SKITY_ERROR_DEVICE_LOST` 的次数与机型分布（lost 的
  直接统计口径，对应排查文档附录三条 present 收尾崩溃的回归观测）；
- `kError` 的次数与构成（按日志里的 VkResult 聚合：尺寸为 0 / OOM /
  其他）；
- `kNeedRecreate` 次数与 presenter 重建成功率；
- context 级重建触发次数（新指标，放量期重点观察是否出现重建风暴）；
- 重建后首帧耗时（重建代价评估）。

## 三、验证方案（与排查文档第六节对应）

1. **Adreno SIGSEGV（20ce1d7a / 4766e349）**：Adreno 830 设备人为制造
   device lost（旋转 / 前后台切换 / 长 GPU 任务压测），预期：
   - 修复前：崩在 `vkQueueSubmit2` 内；
   - 修复后：`Submit` 早退 + 上层收到 `kError`/`kNeedRecreate`，
     触发 context 重建，进程存活。
2. **回归观测**：实验组放量后观察附录三条 issue
   （`4766e349` / `e5f1404b` / `64bc3ea8`）是否归零。
   注意 `64bc3ea8`（`vkDestroyCommandPool` 崩溃）属于异步丢失窗口：
   首次报告点就在崩溃调用内部，客户端早退拦不住，只能靠 context
   重建兜底降低复发概率，预期是次数下降而非完全归零。
3. **validation layer**：在两类机型上复跑，确认无新的非法句柄使用。

## 四、不在本次范围

- descriptor pool 按 pass 新建（P0 另一条，方案需按 fence 门控修正后
  单独实施）；
- 提交路径线程同步（P1，需先确认 `OnReadPixels` 是否存在并发调用方）；
- `OnFlush` 无符号下溢断言（P2，随下次 VK 后端改动带上）。
