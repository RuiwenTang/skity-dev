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
| M1a 行缓冲(可独立合入) | `CoverageRowBuffer`(稠密 u16 行累计)替换双 builder;行末
  `FlushTo` 适配器再生 `vector<Span>`,**下游(clip/brush/blend)零改动**;
  顺带删 per-trapezoid 堆分配、全宽清零、`edges_too_close` 兜底、死代码 delegate |
  现有 sw_canvas_test 5 用例 + 新旧 builder 逐行等价性单测(迁移期) |
| M1b 内核 | Highway 接入 + 28 BlendMode kernel(先吃 post-clip runs)+ 函数表 dispatch +
  Validate 首帧保险丝 + premul 强制;kernel 直连行缓冲与 BrushH 逐像素路径的删除
  随 M2 clip mask 化落地 |
  三实现互拍单测 + `test/bench/micro/sw_benchmarks.cc` 前后对比 |
| M2 结构 | raster clip、glyph A8 atlas、cubic 采样 | unit + 文字基准 |
| M3 验收 | CPU golden 通道(DisplayList→PNG diff);与 GPU/SwiftShader ±1~2/255 交叉容差 | test-runner 增加 CPU 后端 |
| M4 扩展 | DisplayList band 并行、AVX-512 评估、mipmap | 基准驱动 |

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
