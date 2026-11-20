# CPU 软绘后端调研结论(基于 Blend2D v0.21.3 分析)

> 调研时间:2026-11。本文件是跨会话的结论记忆,供后续实施时直接引用。
> 调研对象:本地 checkout 的 blend2d v0.21.3(`/Users/bytedance/Workspace/github/blend2d`)。
> 需求背景:skity 需要完整对齐 GPU 后端能力、性能较好的 CPU 软绘;主力场景为
> **无 GPU 的 Linux KVM 服务端(SSR)**;iOS/移动端作为软绘 fallback 场景仍需可编译。

## 1. 需求与约束(定死的前提)

- **能力**:与 GPU 后端全量对齐(28 种 BlendMode、5 类渐变、图像采样、滤镜、文字)。
- **性能**:主力平台 x86-64 Linux(KVM,CPUID 直通,AVX2 几乎必有,AVX-512 视宿主)。
- **格式**:目标格式基本只有 RGBA8888(premul)。
- **平台红线**:iOS App Store 禁止运行时 JIT(W^X + 私有 entitlement)→ **不能引入 asmjit/JIT**。

## 2. Blend2D 三支柱分析结论

| 支柱 | 位置 | 核心思路 | 对 skity 的价值 |
|---|---|---|---|
| 解析光栅化 | `raster/` | AGG 派生 cell-based signed-area AA,8.8 定点纯整数,cell 编码 `(cover<<9)-area`;边按 band 分桶(高度 8-64,cell 缓冲 ≤256KB);bit 向量(1bit=4px)跳空白;合成时顺带清零 cell | 算法层可参考;skity 现有 AOSP 扫描线光栅器质量等价,**保留不换** |
| JIT 合成管线 | `pipeline/jit/` | 32-bit Signature(dst fmt+src fmt+compop+filltype+fetchtype)→ asmjit(UJIT)把 part 树内联成单函数;函数签名 `void(ContextData*, fill_data*, fetch_data*)`;批宽协商 1/4/8/16/32;AVX-512/AVX2 掩码尾 | **只借组织形式,不借 JIT 本身**:签名 dispatch、part 分解、化简规则 |
| Band 多线程 | `raster/worker*` | 批量提交+flush 集中执行;两遍调度(job 级原子取号 → band 级 interleaved+命令重入);量化 y0 的 SIMD 命令过滤;用户线程参与 | 二期参考(DisplayList 按 band 重放) |

关键区分:**blend2d 的「SIMD」是两条独立机制**——
1. 编译期 SIMD 抽象层(`simd/`,`Vec<W,T>` 模板 + 按文件后缀多 TU 实例化 + 一次性函数表 dispatch),只用于 codec/pixelops 等固定算法;
2. 运行期 JIT 生成向量化合成代码,用于解决合成管线的**组合爆炸**(格式×混合×采样 = 数百万实例)。

skity 格式维度塌缩后组合数仅 ~1300,静态模板可展开 → **不需要 JIT**;多 flavor 静态编译就是「iOS 合法的 JIT 替代品」(编译器在构建期做 asmjit 在运行期做的事)。

## 3. 已定决策

1. **不用 asmjit**(三重否决:组合数不需要 / x86-ARM SIMD 各有免费基线 / iOS 政策禁止)。
2. **SIMD 抽象**(已定:**Google Highway**,2026-11 评审通过;自建薄层方案仅作
   本文档的历史参考,不再实施):
   - 协议:Apache-2.0 OR BSD-3-Clause 双许可,与仓库 Apache-2.0 同源,vendor 保留
     LICENSE 即可,无合规动作;
   - **Highway**:C++17(与 skity 一致)、Apache-2.0、以 header 为主(habitat 引入);
     一个 kernel 写一次(长度无关 + tag dispatch)自动产出 SSE4/AVX2/AVX-512/NEON/SVE
     多份代码;**内建运行期 dispatch**(HWY_DYNAMIC_DISPATCH,每 TU 多目标编译,
     MSVC per-function pragma),自带 CPU 检测/坏 CPU 规避,`hwy::DisableTargets()`
     直接实现 AVX-512 默认关闭;像素惯用法齐全(TableLookupBytes/PromoteTo/
     OrderedDemote2U8/IfThenElse/SaturatedAdd/AverageRound);JPEG XL/Skia 生产验证。
   - **兜底(零依赖时)**:自建 `src/render/sw/simd/` 薄层——128-bit 基线(SSE2/NEON,
     均为平台保证,零运行期检测)+ scalar 数组参照(CI/TSan/oracle)+ `_avx2` 后缀 TU
     + 一次性函数表级联(捆绑判定 AVX2=BMI+BMI2+POPCNT);形状取 blend2d simd 层 10%。
   - 无论选哪条,算法层纪律不变:**语义 op 而非 intrinsic 别名**(div255/mulh_u16 是
     最大分歧点)、**不依赖 movemask**、掩码统一全 1/全 0 向量、数据表示定为
     「px packed RGBA + u16 lane 展开」。库只解决指令映射,不解决算法可移植。
   - **体积实测**(Highway 1.4.0,clang -O2,12 kernel spike,2026-11):库运行时本体
     仅 ~0.7KB(targets.cc);增量模型 = kernel 代码 × 目标副本数。x86 基线为 SSSE3;
     默认动态目标集 = SSE4/AVX2/AVX3/AVX3_DL/AVX3_SPR/AVX3_ZEN4(7 副本,其中 4 份
     AVX3 家族对 128-bit kernel 冗余);`HWY_DISABLED_TARGETS` 位掩码可裁剪,推荐
     服务器构建禁 AVX3 家族(3 副本)。按 ~150 个 kernel 外推:x86 默认 ~300-500KB、
     裁剪后 ~100-200KB、arm64 单副本 ~30-60KB;与自建 SSE2+AVX2 双 flavor 体积同级。
     另:多目标副本由 `foreach_target.h` 单次编译内 pragma 切换产出,无需构建系统
     分 TU(比 blend2d 后缀 TU 机制干净)。
   - **podspec 集成(实测,2026-11)**:vendor 固定 tag 的源码到 `third_party/highway/`,
     subspec 拾取 `hwy/*.{h,cc}`(运行时仅 targets/per_target/abort 三个 .cc,其余
     exclude),`header_mappings_dir` 指向 vendor 根以保持 `<hwy/…>` 包含路径;
     iphoneos arm64 与模拟器 x86_64(pragma 多目标,6 动态副本)均实测编译通过,
     零额外宏;hwy 头不得进 module map(guard 翻转与 C++ modules 冲突);
     `-fno-exceptions` 工程需额外定义 `HWY_DISABLE_EXCEPTIONS`。
3. **管线结构**:两段式(fetch→span→composite)为主——gradient/pattern 走 span buffer;**solid/文字(A8 mask)/boxA 走一段式特例**(solid 常量进寄存器);SrcCopy+solid→memset、SrcCopy+对齐 blit→memcpy 特例保留。signature dispatch + context 级 16 项 SIMD 查找缓存照搬。
4. **多线程**:SSR 主答案 = **进程级并发(每请求单线程渲染)**,DisplayList 录制/回放分离已是现状,零新增机制;band 级并行(DisplayList 重放重入)二期做。
7. **打包分层(2026-11 定稿)**:kernel 管线做成**可选加速器**,核心库永远只带旧路径
   (零增量)+ 探测胶水。三档体积:0(legacy)/ ~30-60KB(slim 单目标)/
   ~100-500KB(多目标,服务器构建裁 AVX3 家族后 ~100-200KB)。
   实际平台版图(Linux/Windows 源码编译无体积约束):
   - Linux/Windows:默认编入 full 变体;SSR 将来要按宿主 CPU 代际分发时,
     同一 seam 直接加 .so,暂不做;
   - iOS:podspec subspec 编译期裁剪;
   - Android:**双 AAR 产物**(base / accel)编译期控制,宿主选依赖;纯拆 .so
     不省体积(APK 仍计入),DFM 按需下发才真省但要求宿主 module 化,不作首选;
   - 实现红线:编译期裁剪必须**排除源文件出编译**(kernel 被函数表引用,
     dead-strip 剥不掉);.so 方案保留为 seam 的可选包装,当前无启用场景。
   ABI:单入口 C 接口 + vtable + abi_version,调用粒度整段 fill-row(fetch 参数
   POD 化);.so 内 Highway 符号 hidden 防进程内符号冲突;Linux 下 accel .so
   静态链 libstdc++。旧 brush 路径定位 compatibility-only 冻结,目标态下线,
   体积档位最终只由 Highway 编译目标数表达;注意回退档存在功能降级
   (advanced blend),需写进接口文档。
5. **光栅化器**:保留现有 `sw_raster`(AOSP 扫描线,梯形解析覆盖,与 cell 法信息量等价,中段天然 CMask 快路径),**重写消费端**。
6. **格式约定**:内部强制 premul RGBA8888;unpremul 只在导出边界一次性转换。

## 4. skity 现状诊断(src/render/sw/,2026-11 分支 capi-hpp-wrapper)

- 已是活的后端:Canvas NVI 三实现之一(HWCanvas/SWCanvas/RecordingCanvas),DisplayList 后端无关、可录制后任意回放;
- 瓶颈/缺口(改造点):
  1. 逐像素 `Bitmap::GetPixel/SetPixel`(switch+shared_ptr 解引用)→ 换批处理 kernel;
  2. **x86 零 SIMD**(仅 `SKITY_ARM_NEON` 路径)→ 新建 simd 层;
  3. Advanced blend 13/28 缺失(`blend_mode.cc:129` 降级 SrcOver)→ 按 GPU 侧 `GenBlendFunctionWGSL` 同一公式源补齐;
  4. clip O(n·m) span 线性扫描 → raster clip 数据结构;
  5. 文字逐 glyph path 光栅化 → A8 glyph atlas(复用 ScalerContext;CPU 不需要 SDF);
  6. cubic 降级 linear、无 mipmap;dst 默认 unpremul 每像素往返;
  7. 单线程立即模式(二期 DisplayList band 并行);
- golden 测试现只跑 Metal(需新建 CPU golden 通道作为「对齐 GPU」的验收设施);
- C API `surface_c.cpp:115` 只认 GL/VK/MTL,需加 CPU surface 分支。

## 5. span 管线评估结论(2026-11 代码级评审,src/render/sw/)

**结论:需要改造,核心是数据模型问题,不是局部优化。**

关键洞察:生产端 `blit_aaa_trapezoid_row` 本来就构造稠密 `alphas[]`,随即被打碎成
RLE `Span{x,y,len,cover}`,消费端 `BrushH` 又解压回稠密 `pm_colors[]` —— 两次转换全是
开销;且 RLE 与逐像素 AA 覆盖天然不匹配(斜边处 `Flush()` 等值合并全部失效,退化为
每像素一条 16B 记录,`sw_raster.cc:44-52`)。

问题清单(热路径行号):
- `sw_raster.cc:44-52` AA 中段逐像素 push_back;`:104` 行切换全宽清零(且为 path bounds 宽);
  `:390/453` 宽梯形逐行 new[]/delete[];`sw_span_brush.cc:121` 消费端每 span 堆分配;
- clip(`sw_canvas.cc:44-266`)逐 span 线性扫 O(n·m)、嵌套 find_span_y、逐 span sort、临时 vector 按值;
- blend mode 运行期值逐像素 28 分支 switch(`PorterDuffBlend`),从未段级特化;
  `BlendPixel` 逐像素 GetPixel(switch+shared_ptr)/SetPixel,x86 无批量;
- gradient 逐像素浮点(矩阵映射 + stop 线性搜索 + 浮点 tile),无 LUT 无增量求值;
- 质量 bug:`PerformMerge` 自带 TODO 承认不正确(`:198`);clip 相交 span 级 `min(cover)`
  对 AA 渐变覆盖是错误近似(`:239`);`safely_add_alpha` 饱和加截断(应超精度累计后量化)。

改造决策:
1. **保留** WalkEdges/AEL/边结构/梯形解析数学(质量 OK);
2. **替换输出表示**:RLE Span → 稠密行 coverage 缓冲(u16 累计 + 行末统一量化);
   `blit_*` 直接写行缓冲;消费端按行 batch(恒定段→CMask 快路径,变化段→SIMD kernel);
   行带 minX/maxX 跳空。此改造即 M1 的第一个交付物,与 kernel 是同一件事的两端;
   walker 现有的排序性质(y 严格单调、行内 x 升序)保证行缓冲**单遍写入、无需排序**;
   u16 累计同时解决:1/4 子扫描线对同一 device row 的多次贡献、饱和加截断精度问题;
   内部 run 信息行末对量化结果做等值 run 扫描(SIMD compare)或 walker 直接标注整数区间;
3. **clip 两步走**:一期 rect clip fast path + 现有 span clip 暂留;二期 path clip 改
   A8 coverage mask 离屏(Skia 同款,统一走 mask 合成,顺带修掉 PerformMerge/min(cover) 两个 bug)。

## 6. 里程碑(原第 5 节,M1 已并入 span 改造)

| 阶段 | 交付物 | 验收 |
|---|---|---|
| M1a 行缓冲(可独立合入) | `CoverageRowBuffer`(稠密 u16 行累计)替换双 builder;行末
  `FlushTo` 适配器再生 `vector<Span>`,**下游(clip/brush/blend)零改动**;
  顺带删 per-trapezoid 堆分配、全宽清零、`edges_too_close` 兜底、死代码 delegate |
  现有 sw_canvas_test 5 用例 + 新旧 builder 逐行等价性单测(迁移期) |
| M1b 内核 | Highway 接入 + 28 BlendMode kernel 直接吃行缓冲 + signature dispatch +
  premul 强制;删除 FlushTo 适配器与 BrushH 逐像素路径 |
  三实现互拍单测 + `test/bench/micro/sw_benchmarks.cc` 前后对比 |
| M2 结构 | raster clip、glyph A8 atlas、cubic 采样 | unit + 文字基准 |
| M3 验收 | CPU golden 通道(DisplayList→PNG diff);与 GPU/SwiftShader ±1~2/255 交叉容差 | test-runner 增加 CPU 后端 |
| M4 扩展 | DisplayList band 并行、AVX-512 评估、mipmap | 基准驱动 |

## 7. 性能预期(机制推算,待 M1 实测校准)

- 合成端(逐像素标量→AVX2 批处理):微基准 8-16×;
- 整体场景 3-10×,叠进程级并发近线性;
- 对照:静态 SIMD 方案 ≈ blend2d JIT 的 85-95%(arm64 上 JIT 也只有 128-bit NEON,差距仅内联/常量折叠;两段式 span 税只落在 gradient/pattern 上),绝对量级对齐 Skia CPU raster;
- blend2d 自身 no-JIT 构建是纯标量管线 → 新 skity 软绘应显著优于「blend2d 的 iOS 形态」。

## 8. Blend2D 关键文件索引(回查用)

- 签名/dispatch:`pipeline/pipedefs_p.h:117-260`(FetchType 29 值、Signature 位布局)、`pipeline/piperuntime_p.h:101`(16 项 SIMD 查找缓存)
- one/two-stage:`pipeline/pipedefs_p.h:370-388`(`DispatchData`,fetch_func==nullptr 即 one-stage)
- JIT 编译与缓存:`pipeline/jit/pipegenruntime.cpp:298-407`;静态全量展开先例:`pipeline/reference/fixedpiperuntime.cpp:102-126`
- 化简规则(memset/memcpy 塌缩):`pipeline/jit/compoppart.cpp:217-255`
- 光栅化:`raster/analyticrasterizer_p.h:149`(类注释)、`:1203`(cell_merge);band 高度:`raster/rastercontext.cpp:4154-4193`
- SIMD 抽象:`simd/simd_p.h`(架构分发)、`core/api-internal_p.h:125-194`(BL_BUILD_OPT/BL_TARGET_OPT 双宏)、CMake 后缀注入 `CMakeLists.txt:928-944`、级联 dispatch 先例 `core/pixelconverter.cpp:1632-1647`
- 多线程:`raster/workerproc.cpp:260-352`(两遍调度)、`:73-160`(量化 y0 SIMD CommandMatcher)
- CPU 检测(asmjit CpuInfo + 捆绑规则):`core/runtime.cpp:128-168`
