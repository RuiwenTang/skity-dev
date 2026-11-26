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
7. **打包分层(2026-11 定稿,dlopen 已剔除)**:kernel 管线做成**编译期可选加速器**,
   核心库永远只带旧路径(零增量)。三档体积:0(legacy)/ ~30-60KB(slim 单目标)/
   ~100-500KB(多目标,服务器构建裁 AVX3 家族后 ~100-200KB)。
   实际平台版图(Linux/Windows 源码编译无体积约束):
   - Linux/Windows:默认编入 full 变体;
   - iOS:podspec subspec 编译期裁剪;
   - Android:**双 AAR 产物**(base / accel = 整库两档编译)编译期控制;
   - 实现红线:编译期裁剪必须**排除源文件出编译**(kernel 被函数表引用,
     dead-strip 剥不掉)。
   **内部结构(精简后)**:不做 C ABI/vtable/加载胶水/POD 序列化——skity_sw_accel
   静态编入,直接 C++ 调用;dispatch 就是 `kernel_table[kBlendMode]` 函数指针数组,
   无需 signature 缓存;`Validate()` 保留为**首帧保险丝**(std::call_once,失败禁用
   加速器回退旧路径):一份二进制分发到异构 KVM 机群,CI 无法覆盖所有目标副本
   (如 AVX-512 副本),首帧校验的正是本机将执行的那份副本。将来的正交补充:CI
   目标矩阵(DisableTargets 逐副本压测)。旧 brush 路径定位 compatibility-only
   冻结,目标态下线;注意回退档存在功能降级(advanced blend),需写进接口文档。
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
| M1a 行缓冲 ✅(59ab9ba4..bf71de5f) | `CoverageRowBuffer`(稠密 u16 行累计)替换双 builder;行末
  `FlushTo` 适配器再生 `vector<Span>`,**下游(clip/brush/blend)零改动**;
  顺带删 per-trapezoid 堆分配、全宽清零、`edges_too_close` 兜底、死代码 delegate |
  现有 sw_canvas_test 5 用例 + 新旧 builder 逐行等价性单测(迁移期) ✅ |
| M1b 内核 ✅(e9278bef..fb471df4) | Highway 接入 + **28 BlendMode kernel 全表**(coeff=整数 SIMD,
  可分离 advanced=f32 SIMD,HSL=标量内核)+ 函数表 dispatch + Validate 首帧保险丝 +
  premul 强制 + gradient LUT 加速器(512 项 + 跨 draw 缓存)+ 宽度泛化(128/256/512)|
  三实现互拍单测 ✅ + `test/bench/micro/sw_benchmarks.cc` 前后对比 ✅
  (x86 -66%,arm64 位精确;kernel 直连行缓冲与 BrushH 逐像素路径的删除
  随 M2 clip mask 化落地。注:M1b 的场景级 A/B 数字见下方 M2-0 勘误——
  运行期开关双镜像使其 legacy 臂无效,纯混合微基准不受影响) |
| M2 结构(2026-11-24 定稿) | **M2-0 测量/回退设施修复**(开关单镜像、梯度标量回退、
  blit_two_alphas 语义回归、行缓冲重叠折叠、arm NEON 混合统一)✅ →
  **M2-1 图像采样** ✅(标量 cubic 镜像 GPU fast-bicubic + SIMD nearest/bilinear
  fetch/blend 引擎 + 逐像素 src 混合引擎提取复用,`cover & global_alpha` 位与 bug
  顺带修,arm NEON 采样统一到标量)→ **M2-2 glyph A8 atlas** ✅
  (软绘切 FreeType mask,复用 AtlasBitmap/PrepareDirectGlyphs)→
  **M2-3 raster clip**(rect fast path + A8 mask 两档,修 min(cover)/PerformMerge)→
  **M2-4 行直连整合** ✅(行缓冲直供消费端、梯度全 28 模式 staged
  fetch→blend、删加速态 BrushH)|
  unit + 软件 golden(`--backend=software`,已全绿)+ 分段/逐次绘制基准 |
| M3 验收 | CPU golden 通道(DisplayList→PNG diff);与 GPU/SwiftShader ±1~2/255 交叉容差 | test-runner 增加 CPU 后端 |
| M4 扩展 | DisplayList band 并行、AVX-512 评估、mipmap。**mipmap 现状
  (2026-11-24 摸底):两侧均未端到端实现——GPU 采样器描述符已传
  MipmapMode(gpu_sampler.cc)但 `FindOrCreateTexture` 构造 TextureImpl
  恒 `mipmapped_=false`,`UploadTextureImage` 永远单 level,
  GenerateMipmaps(vk/gl/web blit pass 已写好)从不触发,请求即被硬件
  LOD clamp 退化为 base-level;CPU `BitmapSampler` 与 M2-1 `ImageParams`
  均不读 MipmapMode。落地必须两侧同步(GPU 接通 mip 链生成 + 纹理缓存
  key 加 mipmap 维度;CPU mip 链缓存 keyed on pixmap、LOD 按 span 预
  计算、kLinear 跨级混合),验收用小图大缩放棋盘(摩尔纹)对 GPU | 基准驱动 |

**M1a 实测记录(2026-11,arm64,commit a4d3cd13)**:
- 关键教训:第一版把不透明内部段也塞进稠密累计,纯光栅化基准回归 11.5×
  (BigTriangle 14.9→172μs)——**常量覆盖段必须 O(1) 直发,绝不能逐像素物化**。
  最终形态为混合表示:AddDirectRun(O(1),legacy 直发语义)+ 稠密 u16 累计,
  flush 按 x 归并;
- 交替 A/B 三轮:端到端场景 parity(premul 1804 vs 1818μs;unpremul 场景两侧
  方差 ±15%,之前的"+6-8% 回归"是测量噪声——**基准结论必须交叉测量**)；
- 纯光栅微基准 +35%(20.3 vs 15.1μs)= 两段式 flush 每行固定开销 ~6.5ns,
  接受:M1b kernel 直接吃行缓冲后该路径整体消失；
- 顺带修复 legacy bug:blit_two_alphas 累计分支把第二像素写到 (x, y+1)。

**M1b 实测记录(2026-11,arm64,coeff 模式于 commit b13fa150 补全)**:
- 内核形态:15 个 coeff 模式(kClear..kScreen,含 kDst no-op)全部位精确对齐
  `PorterDuffBlend`。11 个双项模式共享单引擎
  `result = (s*src_scale)>>8 + (d*dst_scale)>>8`(AlphaMulQ 截断语义),
  scale 常量(来自 sa)或 per-pixel(dst alpha 经 TableLookupBytes 广播);
  Plus=饱和加、Modulate/Screen=MulDiv255Round 精确 u16 复刻
  (`t=p+128;(t+(t>>8))>>8`,p≤65025 全程 u16 内精确);标量尾巴直接调真实
  `PorterDuffBlend`,测试参考亦然——参考与实现不再是两份手抄公式;
- **关键代数结论**:PorterDuffBlend 里 sa/da 为 0/255 的全部快捷分支是统一
  scale 公式的精确特例(`(c*256)>>8==c`,`(c*1)>>8==0`),向量码无需复现分支;
- **premul 是混合求和型模式(SrcATop/Xor 等)的字节安全前提**(per-channel ≤255
  仅对 premul 成立),这是加速器的保证输入域;Validate/测试的 dst 一律折成 premul;
- Release A/B(交替 3×3):SrcOver 场景新旧持平(旧 1631/1669/1677μs,
  新 1694/1668/1675μs,重叠噪声带)——模板引擎化对热路径零代价;
- 教训(测量):**绝不能在 Debug/-O0 构建下做 SIMD A/B**——Highway 内联算子
  全部被 outline 成真实函数调用(反汇编可见循环体内 bl 到 `operator*`/
  `PromoteTo`),同一份代码在 -O0 下呈现 47% 伪回退;A/B 前先确认
  CMAKE_BUILD_TYPE。教训(kRGBA):skity 的 ColorType::kRGBA 内存字节序是
  [R,G,B,A](小端 u32 = 0xAABBGGRR),内核边界统一 PMColorSwapRB 一次进出;
  legacy NEON 路径的 PMColorSwapRB 往返正是同一原因。
- **加速器 on/off 分场景**(commit ff8806b7,运行时开关在同一二进制内交替,
  arm64 Release 3×3 中位数,μs):

  | 场景段 | accel | legacy | Δ | 占全场景 |
  |---|---|---|---|---|
  | BasicShapes | 34.8 | 46.7 | -25% | ~2% |
  | PathEffect | 173 | 188 | -8% | ~10% |
  | DashStroke | 138 | 160 | -14% | ~8% |
  | Text | 416 | 446 | -7% | ~25% |
  | Gradient | 681 | 684 | 0 | ~40% |
  | EvenOddFill | 25.5 | 35.1 | -27% | ~1.5% |

  全场景 1670 vs 1894μs(-11.8%)。**内核在能到达的地方有效(纯 solid 填充
  -25~27%),但收益天花板在覆盖范围而不在内核**:Gradient 完全不进 solid 路径;
  Text 走 `DrawGlyphsInternal`→把字形轮廓当 path 光栅化(非 A8 atlas),
  时间花在光栅化大量小轮廓而非混合。推论:剩余里程碑的收益排序为
  M1b-4 gradient fetcher(~40%)> M2 glyph atlas(~25%)> 其余;
  在这两项落地前,solid 内核再怎么优化对端到端都是个位数百分比。

**M1b-4a 记录(2026-11,arm64,linear/radial + Src/SrcOver)**:
- 设计从"逐像素精确链"中途转向 **512 项 LUT**(Skia/blend2d 同款):精确链的
  逐像素停点搜索 + 动态 gather + 栈往返使其与 legacy 标量打平(实测 0 收益),
  LUT 把检索变为 定点索引 + 2 次标量取数 + u16 有符号增量 lerp;LUT 由
  `BuildGradientLUT` 用精确 legacy 链逐项烘焙(含 tile/硬停点/预乘),
  t 的计算(map/snap/remap/decal)保持与 legacy 逐位一致;
- 精度契约:与 legacy 标量链差 ≤4/通道(网格量化+lerp 舍入+premul 插值次序
  叠加),远小于 GPU 交叉容差;kRepeat 的 t→1 不连续(fract(1)=0 跳回
  colors[0])用**左极限约定**(LUT 末项取 t=1-2/4096),legacy 在 snap 带内
  自身的 1px 翻转语义不复刻;
- 本片连环坑(全部已修,均有单测锁定):①`SWSpanBrush::Brush()` 非虚——
  加速 brush 的覆写从未被调用,加速臂静默跑 legacy;②decal/edge/last 三个
  早退的覆盖顺序,decal 必须最外层;③kRepeat 包裹接缝的 LUT 跨跳变插值;
  ④`1.0f-FLT_EPSILON` 舍入回 1.0f,且 legacy 的 snap 带宽 1/4096 吞掉一切
  ulp 级左极限——左极限必须在 snap 带外取;⑤min/max 化 lerp 把权重镜像
  (下降通道算成 true(1-f)),f≈1 时落到错误端点——必须用有符号增量+
  算术右移;⑥加速 brush 的 LUT 构建在开关检查之前执行,legacy 臂白交税;
- **既有的跨架构不一致(待决策)**:arm 的 legacy NEON 混合
  (`PMSrcOverNeon` 等)与标量 `PorterDuffBlend` 在 28.6% 的 (sa,d) 组合上
  差 ±1(穷举验证)——x86 与 arm 的 legacy 渲染本来就不一致。accel 锚定
  标量语义,开启 accel 反而使 CPU 输出跨架构确定;是否把 NEON 统一到
  标量语义(改变 arm 既有输出 ±1)留给单独决策;
- 附带发现:Highway 的 NEON `Sqrt` 是 rsqrte+NR 近似(非正确舍入),精确
  链方案下会是隐患,LUT 方案将其吸收;
- 性能(arm64,M 系列):**持平**——Gradient 段两臂均 ~175μs。该机器标量
  浮点极强,legacy 逐像素链本来就快;收益预期要在部署目标 x86 KVM 上
  验证(标量吞吐相对向量更弱,预期收益更大),列入 M1b-4d;
- 端到端测试改为容差(accel vs legacy ≤4/通道,含 NEON ±1 叠加),
  fetch 级独立标量镜像 60 轮随机配置 ≤4,Validate 保险丝扩至 4 个梯度
  内核(容差 4,repeat 接缝带跳过)。

**宽度泛化与 x86 首次实测(2026-11,commit b52d54fb/99b4c089)**:
- 内核改为宽度无关(`ScalableTag`):同一源码 NEON/SSE=128-bit、AVX2=256、
  AVX-512=512 实例化;shuffle 控制改算术构造(`(iota&~3)+byte`,宽度无关),
  LowerHalf/UpperHalf+Combine 结构天然宽度无关,钉死宽度的只有 tag/表/定长
  数组。chunk 循环方案在 NEON 上回退 20%(2×8B 载存替代 1×16B)后弃用;
  arm64 实测与 128-bit 版逐位一致、性能持平;
- **x86 首次实测(Core Ultra 7 255H,MSVC,dispatch SSE4)——部署目标兑现**:
  premul 场景 **1545μs vs 4508μs legacy = -66%**(对照 arm64 仅 -12%),
  纯混合微基准 **9.8-13.2×**。证实了"x86 legacy 的 fetch+blend 两半都是
  纯标量(无 NEON 路径),向量化收益远大于 arm"的预判;
- MSVC 工程三连修(99b4c089):①`SW_ACCEL_API` 复用 `SKITY_API`——MSVC 的
  WINDOWS_EXPORT_ALL_SYMBOLS 不从静态库填导出表,必须让 dllexport 指令
  随归档成员进入 skity.dll;②micro bench 链接顺序(skity.lib 亦提供开关
  符号,后扫归档触发 LNK2005);③**MSVC 禁用 AVX2 目标**:多目标 TU 必须
  按基线 ISA 编译(MSVC 无 per-function target 切换),该模式下 MSVC 以
  128-bit 建模向量状态——AVX2 内核把每个 256-bit 值拆成两个 16B 栈存再
  32B 重载,无法 store-forward(~15-20 cycle/处,SrcOver 引擎每 8px 三处),
  实测 AVX2 0.5 Gpx/s vs SSE4 3.6 = **7× 回退**;`/arch:AVX2` 全 TU 可达
  5.6 但会把 VEX 编码泄漏进 SSE 拷贝,不可用。后续正解 = blend2d 式
  **每目标单独 TU 编译**(CMake 后缀注入,AVX2 拷贝独立 /arch:AVX2),
  列入 M4;
- Windows 上 `*_Legacy` A/B 臂语义失效(开关切的是 exe 内静态副本而非
  DLL 的),对比测试需在 macOS/Linux 跑;
- **LUT 跨 draw 缓存**(`AcquireGradientLUT`):thread_local 8 槽轮转,
  键 = 停点表 + tile(哈希快筛 + 全量比对防碰撞),**矩阵不参与键**——
  动画渐变(同停点、变矩阵)逐帧全命中;指针仅保证当前 draw 有效(文档化
  契约)。消除了每帧每渐变 512 次标量链烘焙(arm64 ~6% 段时间;x86 上
  标量链更慢,收益更大)。arm64 实测持平(166-167 vs 167-168μs);
- **多线程注意(M4 band 并行时)**:thread_local 使缓存结构本身无竞争,
  但指针契约是"获取线程内、当前 draw 内有效"——**禁止把带 lut 指针的
  params 跨线程分发**(获取线程继续取新渐变会轮转覆写槽,使用线程读到
  别的渐变颜色,静默错色不崩溃)。并行接法:worker 各自 Acquire(每唯一
  渐变每线程一次构建,缓存跨帧保温)或交付时拷贝 2KB 表;除非 profiling
  证明两者皆痛,不引入共享引用计数缓存。

**M1 收尾记录(2026-11,28 模式全表完成)**:
- **标量参考先行**:advanced 模式此前在 CPU 上回退 kSrcOver(LOGE)+私有
  unpremul SoftLight,与 GPU 不一致。现 `PorterDuffBlend` 将 14 个 advanced
  模式(Overlay/Darken/Lighten/ColorDodge/ColorBurn/HardLight/SoftLight/
  Difference/Exclusion/Multiply + 4 HSL)全部路由到 premul f32 域的
  `AdvancedBlend`——逐表达式镜像 GPU 的 WGSL 生成代码(含 component 助手
  的 guard、SrcOver 结果 alpha、HSL 的 set-color-luminance/saturation);
  量化 = clamp[0,1] × 255 + 0.5 截断,与内核逐位镜像。arm NEON 路径对全部
  advanced 模式旁路(它对未实现模式静默不写,原守卫只挡 SoftLight);
- **内核分三档**:coeff 模式 = 整数 SIMD(位精确,14×);可分离 advanced =
  f32 SIMD(交错布局 + alpha 广播 + 逐 lane select,除法用 IEEE 精确向量
  Div、guard 的未选分支产生的 Inf/NaN 被 select 丢弃——与标量 select 同构;
  位精确,SoftLight 因 Highway NEON Sqrt 近似为 ±1,保险丝表有意不含它);
  4 个 HSL 跨通道 = 标量内核(直接调 PorterDuffBlend,位一致由构造保证,
  向量化留待确有需求时);`SupportsSolidMode` → 全部 kLastMode;
- 实测(arm64):Overlay 3.0×、ColorDodge 2.7×(f32 域 + 转换固有开销,
  对比整数域 14×);28 模式 × 20 轮随机位精确矩阵通过;
- **M1 至此完整交付**:M1a 行缓冲、M1b 全模式内核表 + 函数表 dispatch +
  Validate 保险丝 + premul 强制 + gradient LUT 加速器,x86(-66%)/arm64
  双平台验证。遗留:M2 结构项(glyph atlas/clip/行直连)、sweep/conical、
  AVX2 独立 TU(M4)。

### M1 整体状态(2026-11 关闭)

**已交付并验证**:
- 光栅化前端:`CoverageRowBuffer` 混合行缓冲(O(1) 常量段直发 + 稠密 u16
  AA 累计),legacy 双 builder 与已知 bug(blit_two_alphas 错位)一并清除;
- 合成后端:Highway 加速器 `skity_sw_accel`,**全部 28 个 BlendMode 有内核**
  ——15 coeff 模式整数 SIMD(位精确,纯混合 14×)、11 可分离 advanced 模式
  f32 SIMD(WGSL 对齐,3×)、4 HSL 标量内核;函数表 dispatch + 运行时开关
  (仅 2 个导出符号)+ Validate 首帧保险丝 + premul/kRGBA 门卫;
- 梯度:linear/radial LUT 加速(512 项精确链烘焙 + 跨 draw 线程本地缓存,
  矩阵不参与键);sweep/conical 暂回退 legacy;
- CPU 标量参考:advanced 模式从"SrcOver 回退"修正为 WGSL 逐表达式对齐
  (这是 GPU 对齐的必要修复,也是 advanced 加速的对照基线);
- 工程面:MSVC(导出/链接/AVX2 禁用)、iOS/Android 打包路径(编译期开关)、
  宽度泛化(NEON/SSE 128-bit 实测,AVX2/512 已实例化待 MSVC 独立 TU 启用)。

**验证状态**:arm64(位精确矩阵 + 全量单测 1269/1274,仅剩 Arena 环境项)、
x86/MSVC(场景 -66%、纯混合 9.8-13.2×,Core Ultra 7 255H)。

**有意遗留**(非阻塞,均有记录):M2 结构项(glyph A8 atlas、raster clip、
kernel 直连行缓冲——Text 段 ~25% 是下一个最大杠杆)、sweep/conical、
AVX2 独立 TU(M4)、Windows 上 *_Legacy A/B 臂失效、两个待决策项
(arm NEON 统一、Color4fToColor 舍入等 legacy 瑕疵)。

### M2-0 记录(2026-11-24,测量/回退设施修复,先行切片)

M2 动工前先修测量设施,过程中发现 **三处 M1 记录与事实不符**,均已证实、
修复并有单测锁定:

1. **运行期开关双镜像(自 f54a5a32 起失效,不限 Windows)**。
   `skity_micro_bench`/`skity_unit_test` 静态链接 `skity_sw_accel` 且排在
   `libskity` 之前,exe 内自带一份 `SetEnabled/IsEnabled/g_runtime_enabled`,
   而渲染器(libskity 内)读另一份——`*_Legacy` 基准臂和端到端 A/B 测试
   切的永远是 exe 本地副本。**M1 所有场景级「accel vs legacy」数字因此
   无效(两臂都在跑加速路径)**:「全场景 -11.8%」「Gradient 持平」
   「arm64 -12%」及第 6 节分场景表全部作废;纯混合微基准
   (直调 FillSolidSpans)不受影响;x86 -66% 的对照臂来源同样存疑,
   需在修复后的构建上复测。修复:开关状态移入 skity 目标自己的 TU
   (`src/render/sw/accel/sw_accel_switch.cc`,CMake 在 SKITY_SW_ACCEL 块
   `target_sources` 进 skity),加速库只留内核;`FillSolidSpans/
   FillGradientSpans` 不再读开关(调用方门卫),`Available()`(Validate
   保险丝)保留在库内。nm 验收:测试/基准二进制对开关符号均为
   undefined import(来自 libskity)。
   **修复后重测(arm64 M 系列,7 轮中位)**:全场景 1105 vs 7760μs
   (-86%)、BasicShapes 33.8 vs 294μs(-88%)、Gradient 171 vs 1179μs
   (-86%,此前「arm64 梯度持平」即本缺陷所致)、Text 355 vs 528μs(-33%)。
2. **梯度加速 brush 的运行期回退输出全透明**。`AcceleratedGradientColorBrush`
   继承返回透明的基类 `CalculateColor`,开关关闭/保险丝熔断/配置拒绝时
   落入该路径——示例按 S 切 legacy 会丢全部渐变;端到端测试因缺陷 1
   两臂同跑加速而从未发现。修复:构造时持有真实标量
   Linear/RadialGradientColorBrush 作为 fallback;
   `GradientCanvasAccelMatchesLegacy` 增加两条断言:legacy 臂必须真的
   画出渐变(防回退失效)+ 两臂必须存在 LUT-vs-标量差异(防双镜像复发,
   solid 模式两臂逐位相同无法承担此断言)。
3. **M1a 的 `blit_two_alphas` 缩放是语义回归**。legacy 累计路径
   (SpanBuilder)接收 a1/a2 原值(已按子扫描线切片表达);M1a 引入
   `scale_partial_alpha` 先乘 fullAlpha——45° 斜边覆盖减半(实测
   `(110*128)>>8=55`,两条半步累计 110,legacy 为 220)。这是
   ClipPathDifference(62 像素差)、PositionedGlyphs(68 像素差)、
   DrawDegenerateCubic(120 像素差)三个 golden 红例的共同根因。
   已恢复原值直传(仅此一处;blit_single_alpha 的缩放与 legacy 一致,
   保留)。
   同批修复 **CoverageRowBuffer 发射不变量**:direct run 与稠密脏区间
   (或另一个 direct run)重叠时必须折叠进稠密累加器(`FoldDirectRunsIntoDense`,
   O(1) 范围预检保住 AAA 行的常量路径),否则 flush 发出两条重叠 span,
   同一像素被 SrcOver 混合两次(描边角部实测 208+48 双混合变暗)。

**软件 golden 基线判定**(merge-base `5d2af705` worktree 构建 + 三方像素
对比;merge-base 上 4 条基线全部精确复现 → 差异确系分支引入):

| 用例 | 修复后 vs 基线 | 判定 |
|---|---|---|
| ShapeGolden.DrawDegenerateCubic(共享 GPU 基线) | 200 像素全部 ≤2(与 main 相同的 NEON 舍入水平) | 无需重录,通过 |
| ClipGolden.ClipPathDifference | 21 像素 >2(≤50 容差内);残余为 span 合并喂给 spans_subtraction 的边界近似 | 无需重录,通过;M2-3 raster clip 重做时一并消除 |
| SoftwareTextGolden.PositionedGlyphs | 3 像素 ≤2 | 无需重录,通过 |
| SoftwareTextGolden.ClipRect(require_exact) | 42 像素 ±1 = NEON→标量统一的预期输出变化 | 重录 `_sw` 基线 ✅ |

现状:**golden-shape 24/24、golden-text 2/2 全绿**(`--backend=software`)。

**arm NEON 混合统一(M1 遗留决策落地)**:删除 `sw_render_target` 的
`ProterDuffBlendNeon` 路径(BlendPixelH 全平台标量;`blend_mode.cc` 的
NEON 块与声明一并删除;`color_priv_neon` 的采样助手被 PixmapBrush
最近邻路径使用,保留)。arm legacy 输出与 x86/加速路径逐位一致,
「28.6% 的 (sa,d) 组合 ±1」跨架构漂移消除。代价:legacy 图像 1:1 blit
2.3→6.2 ns/px(兼容路径,M2-1 SIMD 图像管线取代)。

**新基准基线**(arm64 Release,accel on,加入 `sw_benchmarks.cc`):
逐次绘制开销 **810 ns/draw**(1 万个 6×5 矩形,M2-3/M2-4 目标)、
缩放线性采样 **38.7 ns/px**(M2-1 目标)、文字 **3.7 μs/glyph**
(M2-2 目标)、1:1 最近邻 blit 6.2 ns/px。

单测 1275 项:1270 通过 + 3 跳过 + 2 失败(既有 Arena Release death test
环境项),新增 `SWCoverageRowBuffer.OverlappingDirectRunFoldsIntoDenseCoverage`。

### M2-1 记录(2026-11-24,图像采样)

**交付**:

1. **`cover & global_alpha` 位与 bug**(`SWSpanBrush::Brush()`):位与改为
   `MulDiv255Round` 乘法折算。仅影响 PixmapBrush(solid/gradient brush 恒传
   global alpha 1.0,`MulDiv255Round(cover, 255) == cover` 恒等):半透明
   paint 下的 AA 图像边缘此前严重过不透明(cover 128 × alpha 128 实际混在
   128 而非 64——部分覆盖 span 直接被吞成 alpha 位模式)。加速梯度路径的
   原始 cover 直传与该恒等式一致,不受影响。
2. **标量 cubic**:`BitmapSampler::SampleUnitCubic` 镜像 GPU
   `wgsl_texture_fragment.cc` 的 fast-bicubic(cubic_weight +
   cubic_sample_1d 每轴折叠出两个加权点 + 4 次双线性采样合成,16 个最近邻
   tap → 4 次双线性);每个子采样点按 tile 模式二次 remap(与 GPU shader 的
   per-sample remap_float_tile 对应);kRepeat 包裹沿用
   `SampleUnitLinear` 的 tap 处理。取代原「cubic 回退 bilinear」。
3. **SIMD 图像引擎**:`ImageParams`(纹理基址/矩阵/tile/filter/alpha type/
   global alpha)+ `FillImageSpans`(nearest/linear × kSrc/kSrcOver 共 4 个
   kernel,HWY dispatch)。uv 变换与 tile remap 走 f32 lane,texel gather 走
   偏移数组 + 标量取字(基线 target 无 SIMD gather),双线性组合逐位复刻
   `SampleUnitLinear` 的浮点链,premul(A phaType)/coverage(AlphaMulQ)/
   blend 折叠走既有 u16 引擎算子。decal 蒙版在原始 uv 上先行判定(与
   GetColor 次序一致);所有索引在 tile 寻址后统一 clamp,溢出/越界 uv 也
   不会越界 gather。Validate 保险丝扩至图像 kernel(2 alpha type × 2 filter ×
   4 tile 组合 × 2 mode × 2 cover,位精确)。
4. **PixmapBrush 挂接**:`Brush()` 覆写把整个 span 列表一次交给加速器;
   构造期判定加速条件(kRGBA 纹理、色彩变换 noop、非 cubic、无 color
   filter、kSrc/kSrcOver、有限矩阵、偏移 fit u32),其余配置(cubic、色彩
   过滤、非 noop 色彩空间、其它 blend)留在标量 sampler 路径。
5. **逐像素 src 混合引擎提取**:`VariableSrcOverStore`(r = s +
   (d*(256-sa))>>8,掩码尾组 + **零填充栈缓冲**)由 gradient/image kernel
   共用;顺带修复 gradient kernel 尾组 `LoadU` 对 span 末尾的越界读(行末
   最后一组可达 vl-rem*4 字节越过缓冲区)。
6. **arm NEON 采样统一到标量语义**(M2-0 混合统一同款决策):删除
   `PixmapBrush` 的 `CalculateImageColorsNeon` 路径——`vmlaq` 的 FMA uv
   数学与标量链差 ulp,arm/x86 legacy 输出本就不一致;加速内核锚定标量
   链后,legacy 臂保留 NEON 只会污染 A/B。`color_priv_neon.hpp` 随之失去
   全部使用者,M2-0 记录中「采样助手保留」的状态就此失效(文件已删除)。

**位精确经验(本轮方法论核心)**:SIMD kernel 与标量链「表达式相同」≠ 位
相同,编译器收缩形态必须逐链匹配(均有单测锁定):

- 组合式 `w00*v00 + w10*v10 + w01*v01 + w11*v11` 被 LLVM **前端**收缩成
  fmadd(fp-contract=on,**-O0 也收缩**),而标量 sampler 的乘与加隔着 glm
  算子函数边界、不收缩 → kernel 侧把四个乘积拆成独立语句(独立语句 =
  独立 full expression,前端不跨语句收缩);
- uv 链 `(fx*m00 + fy*m01) + m02` 的标量版被收缩成
  `fmadd(fx, m00, fy*m01) + m02`(**第一个**乘积融合、第二个乘积先舍入,
  反汇编确认),而 kernel 的 hwy 算子(NEON intrinsics)天然不可收缩 →
  必须显式 `MulAdd(fx, m00, Mul(fy, m01))` 匹配同一形态;MulAdd 参数顺序
  反了(融合第二个乘积)仍是错;
- 验证集同时覆盖 Debug(-O0)与 Release(-O3)构建(SWAccel 10/10 双侧)。

**性能(arm64 Release,`cmake_bench_release`;Debug 构建的数字无效,见
M1b 教训——本轮重蹈后再次确认)**:
缩放线性采样 500×400 **966μs vs 7263μs = -87%**(4.8 vs 36.3 ns/px,
M2-0 锚点 38.7 ns/px 闭环);全场景 premul 986 vs 7958μs = -88%
(M2-0 记录 1105 vs 7760 = -86%,增量来自场景内的图像段接入);
逐次绘制 757 ns/draw、文字 3.44 μs/glyph(M2-0 锚点 810/3.7,噪声带内)。
采样链路级(探针直拍 `FillImageSpans` vs 逐像素 `GetColor` 链,同轮测量,
legacy 臂未计 blend/brush,比例偏保守):linear 1:1 与 2× 缩小
**5.2 vs 29.0~29.6 ns/px = -82%(5.6×)**,nearest 1:1 **1.3 vs
12.2~12.8 ns/px = -89%(9.3×)**;端到端 1:1 nearest blit 973μs/800k =
1.22 ns/px(M2-0 legacy 锚点 6.2 = -80%)。测量注意:Validate 保险丝的
一次性成本(约 30ms)会摊销到进程内第一个基准配置上——采样级 A/B 必须
先空转一轮再计时。

**GPU 基线一致性(probe 实测)**:`image_cubic_catmull_rom` /
`image_cubic_mitchell`(GPU 共享基线):max channel diff = 1,无任何像素
通道差 >2(81.6% / 76.4% 像素逐位相同)——CPU cubic 与 GPU fast-bicubic
的浮点残差全部落在 ±1;cubic 基线无需重录,旧 bilinear fallback 时代的
宽容差通过也升级为 ±1 级一致。

**验证**:golden-shape 284/284、golden-text 8/8 零失败(`--backend=software`,
含 4 个 ImageGolden——nearest/linear 面板直接打 SIMD 路径、cubic 面板打
标量新路径);单测新增 `ImageSpansMatchLegacySamplerChain`(对真实
BitmapSampler + PorterDuffBlend 链位精确:premul/unpremul × 双 filter ×
4 tile 组合 × 双矩阵 × kSrc/kSrcOver × 双 global alpha)与
`ImageCanvasAccelMatchesLegacy`(canvas A/B 双臂位相等:tile-mode shader、
非 sRGB 纹理、半透明 paint、cubic 各走对应路径);Debug + Release 双构建
单测全绿(Release 目录 7 个 ShaderWriter 失败为该目录 WGX_SPIRV_BACKEND=OFF
的配置遗留,与本片无关)。

### M2-2 记录(2026-11-25,glyph A8 atlas)

**方案取舍**(为什么是 mask atlas 而不是"span 缓存"):
1. **表示形态决定混合内循环**——24px 字形 ≈ 50~80 个 1~3px 碎片 span,重放
   span 缓存仍是逐 span 分支 + 标量混合;稠密 A8 行直接吃 SIMD(M2-1 实测
   nearest blit 1.3 ns/px 那一档)。文本是碎片化惩罚最大的负载。
2. **FreeType 原生输出就是 A8 mask**(`FT_RENDER_MODE_NORMAL`),mask 路径的
   "光栅化" = FreeType 渲染 + memcpy 进 atlas;span 缓存要么保留自研扫描
   转换器(正是要删的成本),要么把 mask 再转 span(白绕一圈)。
3. **设施已在**:`src/render/text/atlas/` 的 AtlasBitmap(纯内存打包页)+
   ScalerContext 的 PrepareOwnedImage 是 GPU 字形管线的现成生产链,缓存 key
   (GlyphKey = PackedGlyphID + ScalerContextDesc)已定型;CPU 复用同构,
   span 缓存是两边都要新建的设施。GPU/CPU 对称。
4. span 缓存的正确场景是**大路径反复填充**(M2-4/M4 范畴),与文本的
   "海量重复小图元"是不同形状的负载。

**实现**:
- `SWGlyphAtlas`(进程级、mutex 保护):AtlasBitmap 512×512 A8 页惰性分页,
  miss 时经 `ScalerContextCache → PrepareOwnedImage` 渲染并插入;key = 全量
  ScalerContextDesc(字号/scaleX/skewX/**设备 2×2 变换**/hinting/edging/
  亚像素开关),缩放/旋转文字按矩阵桶缓存,与 GPU 行为一致;
- 落位契约(port 已文档化):packed phase 融进 mask、`origin = bitmap_left -
  phase`,blit 位置 `quantized pen + origin` 恒为整数网格点;
- 准入(run 级):fill + 非 color emoji + 无 shader/color filter + 目标
  premul kRGBA + 无 path 裁剪(HasClip → M2-3 后放开;rect 裁剪经全局 clip
  bounds 天然生效);kTooLarge/kInvalidImage 字形逐个回退 path 管线
  (**path 懒加载**:命中的字形不再 LoadGlyphPath);stroke 文字走原管线;
- 混合:kSrc/kSrcOver 走新 SIMD kernel(`BlendMaskRect`:mask 字节经
  TableLookupBytes lane/4 广播 → MulDiv255Round 折 global alpha →
  AlphaMulQ 折颜色 → `VariableSrcOverStore`;mask==255 的 lane 保色跳过
  折叠——截断会差 1;lane/4 广播依赖 TableLookupBytes 的 128-bit lane
  局部性,与 BroadcastLaneByte 同一算术,任意向量宽度成立),其余模式标量
  `BlendGlyphMaskRect`(sw_render_target,全模式,与 kernel 位精确);
  Validate 熔断扩至 mask kernel(位精确);
- Debug 构建下 golden 与单测同样有效(本轮 mask 折叠全为整数运算,无
  M2-1 那类 fma 收缩敏感点)。

**测量教训(重要)**:`test-runner.py` 的 golden 套件默认 `--backend=metal`,
**不带 `--backend=software` 时软件专属 case 全部被 skip**——M2-1 记录里
"golden-shape 284/284、golden-text 8/8" 计入的是 GPU 后端 case,软件路径
当时并未真正打到。M2-2 起以 `--backend=software` 的
**golden-shape 24/24、golden-text 2/2** 为准(与 M2-0 口径一致),M2-1 的
图像 golden 在软件后端复验同样通过。bench_release / host_rel 的 CMake 缓存
存在 `SKITY_VK_BACKEND=ON + WGX_SPIRV_BACKEND=OFF` 的不一致状态,CMakeLists
变更触发重配置时才暴露(`cmake -DWGX_SPIRV_BACKEND=ON <dir>` 修复)。

**golden 归因与重录**(M2 计划已批准"接受 FreeType-mask 文字 + 重录基线"):
`--backend=software` 下 PositionedGlyphs 59 像素超容差(max 51,均值 4.6)、
ClipRect(require_exact)36 像素(max 68)——均为 FreeType 光栅化器与自研
扫描转换器在抗锯齿边缘的覆盖值差异,目检实际输出字形/位置/裁剪全部正确后
重录两条 `_sw` 基线。

**性能(arm64 Release,cmake_bench_release)**:
文字 blob(34 字形)**117μs → 7.3μs ≈ 16×**(0.215 μs/glyph,M2-0 锚点
3.7 μs/glyph 闭环;跨构建对比,但量级远超构建方差);全场景 A/B 同二进制
1040 vs 8574μs = -88%(维持 M2-1 水平,场景内文字段收益部分被场景方差吞没)。
单测新增 `MaskRectMatchesScalarChain`(对拍真实标量链,含负偏移表面裁剪)与
`TextCanvasAccelMatchesLegacy`(canvas A/B 位相等 + atlas 反退化断言);
Debug 1274 通过 + Release 12/12(仅既有 Arena 2 项)。

### M2-3 记录(2026-11-25,run-encoded raster clip)

**方案**(讨论定稿,回应"Save/Restore 深栈的内存"关切):裁剪在 clip 时刻
解析成"每像素覆盖",存储走**行程编码**(每行有序 `(x, len, cover)` 段,与
CoverageRowBuffer 输出同构)而非稠密 A8——内存 ∝ 边缘复杂度而非面积,
全屏 path 裁剪 1080p 下 ~10-30KB(稠密 2MB),深栈安全。两档制:
- 第 0 档 bounds(整数 rect 交裁剪,零分配,与既有全局 clip bounds 行为
  一致);第 1 档行程(path 裁剪 / 分数 rect / 混合 op 栈);
- combine 在 clip 时刻一次完成:kIntersect 逐行归并、交叠处覆盖
  `MulDiv255Round` 相乘并拆段;kDifference 逐行减法钳零。leading
  kDifference 从全表面隐式基减起(帧形区域);
- 填充消费统一一条覆盖链:`ClipSpans`(fill span × clip 段,乘法拆段)→
  既有 `MulDiv255Round(·, global_alpha)` → `AlphaMulQ(color, ·)` → blend,
  四类填充(solid/gradient/image/mask)零 kernel 改动;文字走逐行
  staging(glyph mask × stage 行),`HasClip` 门放开;
- 正确性:差分测试锁死——随机 clip op 序列(run/bounds 混合、200 轮)+
  随机 fill spans,行程结果 vs 稠密逐像素参考逐位一致;边界语义
  (覆盖 0 的像素不 blit、小覆盖相乘舍入为 0)与 legacy/Skia blitter 对齐。

**语义变化与 golden 归因(重要)**:AA 裁剪边缘从旧实现的"硬减法"
(`spans_subtraction` 忽略 clip 段覆盖值、整段删除)变为**乘法软边缘**
(Skia AAClip 同款)。ClipPathDifference vs GPU 基线从 21 像素(M2-0 残余)
变为 1000 像素(全 >8,max 255)——M2-0 预言"重做时一并消除"方向错了:
精确的软边缘与 GPU 的**二值 stencil 硬边**差异比近似更大,但这正是
Skia CPU 的语义(CPU 渲染器全程 AA 哲学的延伸),`_sw` 每后端基线存在的
意义就是承载它。重录 `_sw` 基线;**跨后端在裁剪 AA 边缘的分歧(软 vs 硬)
登记为已知语义差异**,M3 交叉容差验收时按边缘带豁免或对齐 GPU。

> **勘误(2026-11-26,main 同步后)**:上文 ClipPathDifference 的归因
> 大部分是错的。真实根因是 `SWCanvas::PushClip` 的 leading kDifference
> 分支把 `DifferenceClip` 参数写反(`diff − 全屏 = 空`),差分裁剪后的
> 蓝色填充**整个消失**(33590 像素差 ≠ "软边语义"),当时误当作软边差异
> 重录 `_sw` 基线、掩盖了 bug。修正参数序后 vs GPU 共享基线仅差 3322
> 像素(2.1%)——这才是软/硬边语义差异的真实量级,M2-0"重做时一并
> 消除"的方向判断其实大体正确。`_sw` 基线已按正确行为重录,并补 canvas
> 级回归单测 `SWCanvas.LeadingDifferenceClipSubtractsFromWholeSurface`
> (既有 `SWRasterClip.LeadingDifferenceFramesTheSurface` 只测了
> `DifferenceClip` 函数本身的参数序,没测 PushClip 接线,故未拦截)。
> 教训:**重录基线前必须先解释每一处差异的机制来源**;量级突变(21→
> 33590)不可能由"边缘近似方式变化"解释。

**顺带退役**:PerformMerge(TODO "not correct")、FindSpan(第三分支 `+1`
off-by-one)、spans_subtraction、RecursiveClip 的 op 匹配逻辑、每次填充的
span 求交。OnDrawPaint 统一走 DoBrush。

**性能(arm64 Release,cmake_bench_release)**:全场景 A/B 同二进制
1009 vs 8136μs = -88%(维持);逐次绘制 805 ns/draw、文字 7.29μs
(0.215 μs/glyph)——锚点全部维持。带 clip 的填充新增逐像素折叠成本,
本场景无 path 裁剪,无回归。
单测新增 `SWRasterClip.*` 5 项(差分 fuzz + 边界 + span 合成 + staging);
`TextCanvasAccelMatchesLegacy` 扩展 path-clip 交/差两种文字绘制;
Debug 1279 通过 + Release 17/17(仅既有 Arena 2 项)。

### M2-4 记录(2026-11-25,行直连整合 + 梯度全 28 模式)

**梯度全 28 模式(staged fetch→blend)**:M2-1 的梯度加速只有 kSrc/kSrcOver
(融合内核取色+混色一趟)。补齐其余 26 个模式走**分级行直连**架构:
- kSrc/kSrcOver 融合内核原样保留(单趟、无往返,不回归);
- 其余模式复用 **kSrc 取色内核做 fetcher**——每 span 先把"稠密颜色行"
  (premul 内存字、cover 已折)取进 512px 栈分级缓冲(>512px 分块),
  再交给 **variable-src 混合引擎**一次混一行:kClear memset / kDst no-op
  在 dispatch 直处理;11 个 coeff 模式 = ScaleKind 表的逐像素 sa 泛化
  (kInvSrcA/kSrcA 由常量变向量,均匀公式与逐像素 shortcut 的等价性
  与 M1b 同理成立);10 个可分离 advanced = f32 引擎,sf/saf 从分级行
  逐像素加载;3 个 HSL 保持标量 PorterDuffBlend 循环;
- `SupportsGradientMode` = 全模式,brush 侧加速门相应放开(sweep/conical/
  color filter/unpremul 仍走 legacy);
- 验证:保险丝 ValidateGradientModes 以**逐像素变化 src**(覆盖 0/255
  shortcut 边界)驱动每个引擎,coeff 引擎与标量混合**位精确**;单测
  `GradientStagedModesBlendOwnFetch` 用 kSrc 跑捕获 fetch 词,断言**所有
  28 个模式**输出 = 对这些词的真实 PorterDuffBlend(coeff/HSL 位精确、
  advanced ±1、SoftLight ±2),覆盖 >512px 分块与 dispatch 接线;
  画布级 `GradientAdvancedModesAccelMatchesLegacy` A/B ±4。

**重要发现(记录进方法论)**:f32 advanced 引擎与标量链存在 **±1/通道**
的最终舍入差——Release(-O2 也含 -O0)标量 `AdvancedBlend` 把
`a*b + c*d` 契约成 fmadd(反汇编证实,342 条),而 Highway 内核显式
Mul+Add 不契约。M1b 的固定位精确保险丝只是测试值集**碰巧**没跨舍入
边界;连续域扫描(随机梯度)必然命中。处理:variable 引擎保险丝对
kOverlay..kMultiply 放宽 ±1(SoftLight ±2 含 NEON Sqrt 近似),coeff
引擎保持位精确(整数构造,结构性)。±1 在 M3 交叉容差(±1~2/255)
之内,GPU WGSL 侧自身也有舍入,不做逐表达式契约形状对齐(M2-1 在
bilinear 上做过一次,成本收益不成立)。

**行缓冲直供消费端 + 删加速态 BrushH**:
- `SWSpanBrush` 退役逐 span 虚函数 `BrushH`(M1 加速时代的扩展点):
  标量回退路径在基类 `Brush()` 内联——span 钳位、cover×global_alpha
  折叠后,稠密颜色行写进**成员复用行缓冲**(此前每 span 堆分配
  `vector<uint32_t>`),一次 `BlendPixelH` 调用交给消费端;
- `SWRenderTarget::BlendPixelH` 增加 premul kRGBA **直连行混合**:
  逐像素 GetPixel/SetPixel/边界检查只在 unpremul 等直连不可服务的
  格式保留;premul 行直接按行指针混合(FastBlend 捷径语义不变,
  PorterDuffBlend 公式不变,输出位一致);
- 加速态不受影响:kSrcOver 融合、图像、mask 内核不变;solid 早已
  `FillSolidSpans` 直连。

**性能(arm64 Release,cmake_bench_release)**:400×300 kMultiply 全屏
梯度 525 vs 1704μs = **-69%(3.25×)**(f32 advanced 数学 + 分级往返,
比 kSrcOver 融合路径的 -89% 低是预期内的)。行直连让**标量回退臂本身**
大减:全场景 legacy 臂 8136 → 4733μs(-42%,BlendPixelH 直连行 +
删每 span 堆分配);加速臂锚点全部维持——全场景 1015μs(M2-3:
1009)、逐次绘制 ~820 ns/draw(805)、文字 7.21μs(7.29)。
单测 15/15 SWAccel 全绿;Debug 32/32(渲染四套件)。

### main 同步记录(2026-11-26,M3-0 前置)

远端 main 在 M2 收尾期间并行补了一批**旧软绘修复 + 软件 golden 扩容**
(f5103227..67c9ff66,#603-#609),已 merge 进本分支。对 M3 规划的影响:

- **main 已占位 M3-1/M3-3 的一部分**:`kSoftwareGoldenFilter` 扩到 ~34
  case(新增 BlendMode.Screen、11 个 ColorSpace、gradient fallback×2、
  Image linear/nearest-colorfilter、RetainedFrames、CullRectClipDrawPaint),
  CI 新增 `osx_software_golden_test` job(shape + ft text,再切 CT text)。
  M3 的清单扩容改为**与 main 协同**:剩余大头是 blend_mode 其余 27 模式、
  save_layer/image_filter 组、skp;
- **main 的修复是针对旧软绘的**,逐个与本分支重写的对应关系:#603
  (DrawPaint 尊重 device clip bounds)分支已有等价实现;#604(color
  filter 组合顺序)共享代码直接生效;#605(NEON screen 饱和)分支已删
  NEON,标量+内核输出与 main 修复后 `_sw` 基线吻合(Screen golden 绿);
  #606(bilinear 坐标 clamp,负浮点转 u32 UB)共享 sampler 生效,分支
  SIMD 内核的 i32 域 clamp 语义本就与修复后一致;#607(CI)/#609(nearest
  采样保留 color processing + software golden surface 标注 target color
  space)的 sw_span_brush NEON 部分整体过时(分支已删),golden env 的
  color space 标注合并生效;#608(退化梯度解析为 tile color)自动合并进
  分支 GenerateBrush(stop 空间转换之后,语义正确);
- **同步暴露并修复一个 M2-3 真 bug**(见 M2-3 勘误):`PushClip` leading
  kDifference 参数反转,`RecorderGolden.DisplayListCullRectClipDrawPaint`
  (共享 GPU 基线)抓到,`ClipPathDifference` 的 `_sw` 基线按正确行为重录;
- 同步后验证:软件 golden shape 42/42、text 2/2;unit 1290(1285 通过,
  2 失败为既有 Arena death test 环境项,3 skip);
- `ImageGolden.NearestPaintColorFilter` 是**共享 GPU 基线**(无 `_sw`),
  即软绘直接对 GPU 交叉验收的先例——M3-2 交叉容差设施的第一块拼图
  已在 main 落地一例(逐 case 手工形态)。

### M3-0 记录(2026-11-26,全量摸底)

绕过 allowlist(`GTEST_FILTER` 环境变量使 `golden_test.cc` 不套默认清单)把
shape 二进制全部 310 case 在 software 后端跑通,归因分类后按类处置。

**本轮交付**:
- **崩溃修复**:`RectGeometryGolden`(5 case)与 `ColorSpaceCoreReviewGPU`
  (4 case)在 helper 里解引用 `GetGPUContext()==nullptr` 直接 SIGSEGV——
  均为 GPU 管线专属测试(rect-batch/MSAA/P3 snapshot),TEST 体加
  software skip guard;注意 `GTEST_SKIP` 是 **return 语义不是异常**,
  有返回值的 helper 无法承载(第一版编译错误),guard 必须落在 TEST 体或
  void 函数内;
- **GPU 机制 helper guard**(blend_mode 6 处):TextureCopy/SaveLayer
  TextureCopy/ImageFilterSaveLayerTextureCopy/ClipReplay/RestoredClipPath/
  SaveLayerClipReplay——config 强制的 GPU capability tier 在 software 下
  no-op,测不到任何东西,统一 skip;`NativeBlend_/FramebufferFetch_` 系列
  已有 cap guard 自动 skip;
- **15 个基础 blend 模式 `_sw` 基线**(Modulate/Overlay/…/Luminosity)+
  allowlist 扩容:**软件 golden 42 → 57 case 全绿**。归因:面板构图 =
  红圆(r=30)+ 蓝矩形直接混合,~150px 差全部落在圆周 AA 边缘带(解析
  AA vs GPU MSAA 覆盖差穿透进混合结果,15 模式等差分布 130-165px 互相
  印证),与 Screen 的 main 先例同机制;`SaveLayer_` 系列(隔离后
  SrcOver 合成)无需 `_sw` 直接过 GPU 基线;
- 摸底终态:**310 case = 104 过 / 70 失败 / 136 跳过**,全量串跑无跨
  case 状态污染(逐 suite 二分验证)。

**剩余 70 失败的四层归因(M3-1 待办清单)**:
1. **第 0 层:0px diff 但断言失败(9 case)**——`StrokeDepthOverlap`、
   `FilledRRect45DegreeAAEdges`、`TextureCopyPreservesTransformedLayer
   Contents`、`P3TargetSaveLayerComposite`、`DeclaredBlendFilterColor
   ResolvesIntoTarget`(blend filter 色彩解析差 12/255)、
   `DeclaredShadowMatchesImageOnSRGBTarget`(**阴影颜色差 127/255**)、
   `RoundTripPreservesDeclaredShadowOnP3Target`、`FailedSaveLayer
   PreservesMatrix`、`NearSingularDrawPaintPreservesShaderCoordinates`。
   测试内数值断言而非像素比对——**软绘色彩空间/阴影/矩阵保持管线的真实
   差距**,优先深挖(修 bug,不是录基线);
2. **第 1 层:≤500px AA 边缘带(~25 case)**——描边/rrect/curve/checker
   类(`StrokeJoinAndCap` 1310、`CatHeadPath` 1355、`ClipRect` 1847、
   Recorder CullRect 系列等):解析 AA vs MSAA 边缘覆盖差,机制同
   blend 面板,批量目检抽验后录 `_sw`;
3. **第 2 层:500-8000px 混合(~15 case)**——`ClipPath` 3544(max 65%)、
   `CoverageAwareBlendModes` 7995、`DrawYinAndYang` 3255 等:量级超出
   边缘带解释范围,逐个看 diff 图归因(部分可能是 conflation/覆盖累计
   语义差,部分可能挂 `_sw`);
4. **第 3 层:>10000px 或 max 100%(11 case)**——`Tiger` 55929、
   `FallbackPathAndRRect` 44408、`TwoCircleWithTranslate` 31962、Blur
   系列 5 个(stack blur vs GPU 两遍模糊的实现差)、`RadialGradient
   Flags` 30184(max 25%)、`LinearGradientWithColorStops` 16276。
   渐变两个疑似色彩空间插值路径差距(与第 0 层 ColorSpace 断言失败
   同族),blur 系列大概率登记实现差异 + `_sw`。

**第 0 层首查线索(DropShadow,2026-11-26,已修复)**:
`DeclaredShadowMatchesImageOnSRGBTarget` 定位到
`DropShadowImageFilter::OnFilter` 三连缺陷:①declared `Color4f` 直接
线性量化当 sRGB(丢空间转换);②`ColorSetA(srgb_color, mask_a)` 用 mask
覆盖度**替换** alpha,declared 半透明 alpha 丢失;③tint 后的
`filtered_bitmap` 默认 premul/sRGB 声明,回读 blit 再 premul 一次、宽色域
target 上再转换一次。修复(commit ad09b35c):tint 走
`ColorTransformSteps::Resolve(declared, layer空间).Apply` →
`Color4fToColor` → `ColorToPMColor`(与 `BitmapSampler::GetColor` 的 image
decode 链逐位同构)→ `AlphaMulQ(shadow_color, mask_a)`;`HandleFilter`
的 temp layer(sRGB glyph 版两处)补 `SetColorInfo(画布空间)`。
`DeclaredShadowMatchesImageOnSRGBTarget` / `LegacyShadowRidesIntoP3Target`
转绿。**排查过程的方法论教训**:(a) 探针中途一度把正确的 encoded 值
(0.9175,0.2002,0.1385 = sRGB red 的 P3 表示)误当 linear 值,推出
"encode 步失效"的假说——数值归因必须先算准一个已知正确值再对比;
(b) `ColorTransformSteps`/`InvertTransferFn`/`EvalTransferFn` 全链验证
无误,真正的 bug 全在调用方(drop shadow 自己),共享层没有问题。
**新登记缺口**:`RoundTripPreservesDeclaredShadowOnP3Target` 的 wire 形态
是 `Matrix > ColorFilter(Blend) > Blur` 嵌套组合,软件渲染器只实现了
Blur/DropShadow 叶子,`MatrixImageFilter`/`ColorFilterImageFilter` 无 CPU
`OnFilter`(基类空实现)→ 嵌套回放 no-op,case 已加 software skip;
**嵌套 image filter 的 CPU 组合执行**列为 M3-1a 后续工作(也是
ImageFilterGolden 的 Matrix/Compose 系列第 3 层失败的候选根因之一)。

### M3-1b 记录(2026-11-26,AA 边缘批量 + GPU 管线段 gating)

- **20 个 AA 边缘 case 录 `_sw` 并进 allowlist(57 → 77 全绿)**:
  描边宽窄(Tiny/Large/Scaled)、TileBoundary、ConcaveSelfIntersecting、
  Curves、CanonicalEdgesExact(746px 全 ≤2 但超 50px 数量门槛)、
  SimpleShape 7 个(rrect 描边/DRRect/filled)、Recorder CullRect 5 个、
  ClipRRectDifference。抽验目检:filled_rrect diff 94% 部分覆盖像素
  (标准边缘带);curves/tiny_stroke 的 diff 像素不透明(覆盖内外判定
  的浮点边界差,同族);
- **GPU 管线段 gating(不是软件路径的段不再跑)**:整 case skip ×6
  (StrokeDepthOverlap=stencil overlap exact、TextureCopyPreserves
  =texture-copy 合成、FilledRRect45=simple-shape 解析边、Conflation×2、
  ClipUsesGlobalBounds=coverage-AA 基线);多段 case 的 GPU 段
  (coverage/contour/simple-shape)改为**条件包裹**而非 GTEST_SKIP——
  教训:已 PASS 的比较之后再 GTEST_SKIP 会把整个 case 报成 SKIPPED,
  掩盖第一段的验证结果;Recorder 的 CompareRecorderGolden helper 对
  coverage_aa 段 early-return;
- **录制工具坑**:`SKITY_UPDATE_MISSING_GOLDEN=1` 会让**所有段**都录
  (含 GPU 变体目录 coverage_aa_images/ 等),多段 case 录制前先加
  gating,或录制后删除变体目录的 `_sw`(本轮删了 11 个误录);
- 全量终态:**128 过 / 38 败 / 144 跳**(M3-0 起点 104/124+2 崩),
  剩余 38 = 第 2/3 层(Blur 族 5、渐变 flags 族 3、ClipPath/ClipRect、
  CoverageAware、Tiger、SaveLayer 大场景等)+ 嵌套 filter 族
  (ImageFilter Matrix/Compose 3 个)。

**M3-1c 第一批(Blur 族,2026-11-26)**:7 个 blur case(Blur×5、
DropShadow_0_0_10_10、ScaledBlurMaskFilter)录 `_sw`。归因:
`BlurBitmapToCanvas`/`DropShadowImageFilter` 都走
`SWStackBlur(round(max(rx,ry)))`——**各向异性半径折叠成对称方核**
(stack blur 的已知简化),vs GPU 可分离精确核,差异 = 晕区的核形状
分布(max 通道差 ~40/255,16% 像素)。非正确性缺口;**两半径可分离
stack blur 扩展**登记为后续可选优化(对齐 GPU 更近)。嵌套 filter
三 case(Matrix_translate/RotateMatrix/ComposeBlurMatrix)随 RoundTrip/
FailedSaveLayer 一起 skip,等 CPU 嵌套执行落地。allowlist 77 → **84**;
全量 **135 过 / 28 败 / 147 跳**。剩余 28 待归因:渐变 flags 族 3
(RadialGradientFlags 3 万 px/25%——疑似色彩空间插值真差距,优先)、
SaveLayer 大 diff 族 3(TwoCircleWithTranslate 3.2 万/PerspectiveZ0Plane/
ThreeCircle)、Shape 中量级 5(CatHeadPath/DrawCheck×2/StrokeJoinAndCap/
PathTransformFillType)、SimpleShape 残余 5(rrect 变换描边 + Coverage
Aware + YinAndYang)、ClipRect/ClipPath、Gamma ColorFilter×2(457px,
疑似 8bit 表量化 vs GPU 浮点)、DisplayP3AndLinearSRGBDecode、
CullRectClipPathTransform、Tiger。

**M3-1c 第二批(渐变 flags 族,2026-11-26,premul 插值域实现)**:
渐变插值的 **premul 域(非零 gradientFlags)此前软件零消费者**——两列
flag 变体渲染相同,半透明 stops 与 GPU 整面板发散(RadialGradientFlags
差 3 万 px)。实现(commit bf882776):`GenerateBrush` 在 stop 空间
resolve 之后 premul 化停点表(镜像 GPU 的 `Premul` uniform 上传,注意
**不 clamp**——GPU 靠 shader 尾部 clamp 收敛);brush 侧
`FinalizeColor4f` 按域量化——premul 域 clamp RGB 到 [0, A](shader 输出
clamp 的镜像,同时把超 gamut 的 resolved stops(如 sRGB target 上的
P3 红)折回规范 premul 域)并跳过尾部 round-premul;LUT 参考链
`ReferenceGradientColor` 同构镜像。**两个连环坑**:①`GradientInfo` 的
字段名是 `gradientFlags` 不是 `flags`(`flags` 是 `GradientOptions`
的);②**LUT 线程缓存键必须含 premul_stops**——不透明/全透明的表
premul 化后字节相同,不同 flag 变体会碰撞取错表(RadialGradientFlags
修好 premul 后 diff 纹丝不动的根因)。验证:
`GradientClampsBeforePremultiplying`(flags 双臂,clamp 语义)+
`RadialGradientFlags` 直接过 GPU 共享基线;LinearGradientWithColor
Stops(硬停点的 LUT 量化噪声 ≤4 契约)、Gradient/ImageShaderLocal
Coordinates(陡峭渐变区的亚像素采样相位,紧邻列 RGB 交错)录 `_sw`。
allowlist 84 → **88**;全量 **139 过 / 24 败 / 147 跳**。渐变族仅剩
`FallbackPathAndRRect`(4×4 fallback 面板,row0 白 vs 蓝白的整块差,
退化渐变 fallback 色选择与 GPU 的语义分歧,待深挖)。
另:stack blur mask 内部覆盖 ~254(非精确 255),是 `EXPECT_EQ(direct,
Color_RED)` 类精确断言在软件下的固有 1 值差来源。

**第 0 层收尾(2026-11-26,9/9 处置完)**:修复 5(DropShadow×2、
`P3TargetSaveLayerComposite`——`LayerState::Init` 的 layer bitmap 补
SetColorInfo(画布空间),commit ba1d422e、`DeclaredBlendFilterColor
ResolvesIntoTarget`——`GenerateBrush` 入口把 declared Blend filter 颜色
resolve 进 target(`ResolveColorFilterForTarget`,镜像 GPU uniform 解析);
**生命周期教训**:resolved filter 曾放局部 shared_ptr,brush 持裸指针,
GenerateBrush 返回后 use-after-free(单跑侥幸过、全量崩)——brush 持裸
指针的约定要求任何替换 filter 的生命周期挂在 canvas 成员上、
`NearSingularDrawPaintPreservesShaderCoordinates`——渐变末端 ±1 lsb,
software 下放宽 ±1(GPU 保持精确));skip 2(RoundTrip、
`FailedSaveLayerPreservesMatrix`——后者确认 **SaveLayer + image filter
的 layer 内容在软件下整体丢失**(MatrixImageFilter::OnFilter 空实现),
是第 3 层 SaveLayer/ImageFilter 族失败的共同根因,嵌套 filter CPU 执行
是它们的统一解);基线类 2(StrokeDepthOverlap、FilledRRect45Degree
AAEdges + TextureCopyPreservesTransformedLayerContents,均 use_backend
specific 缺 `_sw`,归第 1 层批量)。全量 310 case 终态:
**108 过 / 64 败 / 138 跳**,色彩空间管线(Layer 空间/filter 声明色/
阴影声明色)在软件上与 GPU 语义对齐。

**方法论(本轮教训)**:stdout 的 `GOLDEN_TEST_FAILED_METRICS` 解析必须
按"**最后一个** `test name:` 行"归属——正则 `test name:.*?METRICS` 的
非贪婪跨测试串位曾误报 8 个"全量失败/单独通过"的假阳性,引出一次
"跨 case 污染"误判(逐 suite 二分 + JSON 权威列表复核后排除);gtest
json 报告的 failure 列表才是权威。

## 7. 性能预期(机制推算,待 M1 实测校准)

> **校准状态(2026-11)**:以下为开工前的机制推算,现在有实测锚点了。
> **合成端 8-16×:已实测兑现**——纯混合微基准(BM_SWSolidBlend*,固定 span 表
> 直灌 FillSolidSpans vs 同 span 逐像素 PorterDuffBlend,arm64 Release 三轮
> 一致):SrcOver 14.4×、SrcOverOpaque 14.3×、SrcIn 13.3×、Modulate 13.6×,
> 吞吐 ~16.6 Gpx/s vs ~1.16 Gpx/s;
> **"整体场景 3-10×:不成立**——solid 内核接通后端到端仅 -12%(第 6 节分场景
> 表,arm64)。差距来源不是内核不够快,而是推算只按"逐像素标量→SIMD"估了混合一段,
> 没计入 (a)gradient/text 根本不经过被加速的路径(Gradient 40% + Text 25%),
> (b)光栅化与 span 物化占其余大头。混合阶段已不再是瓶颈,后续收益排序:
> M1b-4 gradient fetcher(~40%)> M2 glyph atlas(~25%)> 其余。
> **x86 更新(2026-11,Core Ultra 7 255H/MSVC)**:gradient 加速 + SSE4 宽度
> 接通后,同一 premul 场景 **-66%**(1545 vs 4508μs)——"整体 3-10×"在部署
> 架构上以累计方式接近兑现(1.5-3× 端到端),arm64 的 -12% 是该机器标量
> 浮点过强所致,不是方案上限。
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
