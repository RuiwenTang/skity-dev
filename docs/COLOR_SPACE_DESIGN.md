# ColorSpace 机制设计

> 定稿时间:2026-11(v2,输出空间支持重构)。本文件是跨会话的设计结论记忆,
> 实施细节如与本文件冲突,以实际代码为准。
> 需求背景:skity 需要类似 Skia 的 ColorSpace 机制。初始不追求支持很多颜色空间,
> 但机制必须完整:类型、公共 API、内部颜色转换链路。v2 起新增:**渲染输出结果
> 需要支持非 sRGB 颜色空间**(P3 目标 surface、按空间导出/回读)。

## 0. v1 → v2 变更记录(为什么重构)

v1(提交 `2dbbfd4e`)按"全局工作空间固定 sRGB"实施:非 sRGB 图像在采样时解码到
sRGB,输出永远是 sRGB。新需求(输出支持其他空间)打破的两个 v1 前提:

1. **全局工作空间不成立**:目标可以是 P3 等;若仍固定 sRGB 工作,P3 内容全程被
   sRGB gamut 裁剪,输出转换也只在 flush 末尾打补丁;
2. **Paint 设色时机转换不成立**:`SetColor4f` 设色时转到"工作空间"依赖工作空间
   全局唯一;DisplayList 录制的颜色必须携带空间、在 draw 时解析到目标空间。

v2 保留 v1 的全部资产:ColorSpace 值类型与数学、codec 元数据、C ABI 通道、
输入侧解码块(泛化)。改变的是空间模型与转换的组织方式。

## 1. 现状与目标

现状(2026-11,分支 feat-color-space,基线含 `2dbbfd4e`):

- `ColorSpace` 值类型 + 参数化转换数学已落地(`src/graphic/color_space.cc`);
- 输入侧:非 sRGB 图像 GPU 采样后解码到 sRGB(shader variant)、SW sampler 出口
  解码;Paint 设色时机转换;codec PNG gAMA/cHRM/sRGB chunk 解析与编码;
- 输出侧空白:所有 render target 为 RGBA8/BGRA8 **unorm**(`src/gpu/gpu_texture.hpp`
  的 `GPUTextureFormat` 无 sRGB 变体);混合为固定功能**编码值混合**
  (`hw_blend_plan.cc`);fragment 输出后无任何后处理(无 dither/encode/clamp);
- 呈现直写:最终 draw pass 直接写 swapchain/drawable(MTL/VK)或 blit(GL),
  `ReadPixels` 原始字节回读,均无转换;`GPUSurfaceDescriptor` 无空间字段。

目标:

1. `ColorSpace` 类型与公共 API(v1 已有,保留);
2. 内置 sRGB、线性 sRGB、Display P3 + parametric `MakeRGB`(v1 已有,保留);
3. **每个 render target(surface/layer/snapshot/SW bitmap)携带 ColorSpace,
   颜色与图像在进入目标时转换,混合与存储留在目标空间**;
4. 输出边界:offscreen surface、snapshot、ReadPixels(打标签/可转换)、codec 编码;
5. 线性宽色域 = 目标空间取线性 TF + 浮点格式的模式(机制内含,二期实现);
6. **未指定颜色空间时,渲染行为与现状逐位一致**(零回归要求,继续有效)。

## 2. 决策记录(定死的前提)

### 2.1 ColorSpace 是描述性值类型,不是 shared_ptr 对象(v1 保留)

内部表示 = **7 参数 transfer function(skcms 布局)+ 3x3 RGB→XYZ gamut 矩阵**,
共 16 个 float + 一个 kind 标签,约 80 字节 POD。默认构造 = sRGB,"未指定"与
sRGB 天然同一语义(向后兼容)。`operator==` 按 16 个 float 数值比较。

### 2.2 工作空间 = 目标空间(v2 核心,替代 v1 "固定 sRGB")

- 每个 render target 携带 ColorSpace:**surface(根 layer)、saveLayer 离屏、
  snapshot target、SW Canvas 的 Bitmap**;
- 转换只发生在**进入目标时**:图像采样解码(src→target)、Paint/渐变设色
  (recorded→target,draw 时)、layer 合成(layer space→parent target space);
- **混合与存储留在目标空间**:8888 unorm + 固定功能混合不变(编码值混合,
  与 Skia 8888 目标同类行为;P3 目标 = P3 编码值混合);
- 目标空间即工作空间 ⇒ **输出直写零转换**:工作空间对齐 swapchain/导出声明的
  空间时,present/ReadPixels 无需任何转换 pass(这是对"全局固定工作空间"方案
  的决定性优势);
- **转换分解天然 hub-and-spoke**:`src TF⁻¹ → 线性 src gamut → 3x3 → 线性
  dst gamut → dst TF`。新增空间只是新增常量,不存在 N×N 两两路径。

**目标格式选择规则(v2 定稿):格式跟随"模式",不跟随"色域"**。

| 目标空间 | 模式 | target 格式 | 混合 |
|---|---|---|---|
| sRGB(默认) | 编码 | RGBA8/BGRA8 unorm(现状) | 编码值,字节不变 |
| Display P3 | 编码 | **同为 RGBA8/BGRA8 unorm** + P3 空间声明 | 编码值(同类行为) |
| 任意空间 | 线性模式(opt-in,二期) | RGBA16F | 线性值 |
| HDR/PQ(远期) | — | RGBA16F | — |

依据:Display P3 = P3 原色 + sRGB 传递函数,8bit 编码存储/呈现完全成立——各平台
P3 swapchain(Vulkan `DISPLAY_P3_NONLINEAR_EXT`、WebGPU `'display-p3'`、macOS
P3 display)均为 8-bit unorm 形态;宽色域只改变通道语义,不改变位深需求。现状
默认混合在 sRGB **编码**(gamma)空间而非线性空间,故 8888 够用(线性值放 8888
暗部 banding)。RGBA16F 仅三种 opt-in 触发:线性混合质量模式、HDR/PQ(8bit
精度不够)、需保留超界中间值的跨空间合成(边缘场景)。

**线性宽色域模式(二期)**:目标空间取 `线性 TF + 宽 gamut`、格式取 RGBA16F,
即得"浮点线性混合 + 输出编码"的完整宽色域行为——同一机制的一个取值,不是独立
架构。CPU 软绘内部强制 8888(见 `CPU_RENDERER_RESEARCH.md` §3),线性模式在
SW 后端延后。

**sRGB 硬件格式备注**:三大 API 的 sRGB 编码附件格式(VK `*_SRGB`、GL
`SRGB8_ALPHA8`、MTL `*Unorm_sRGB`)硬件自动 decode→blend→encode,**只覆盖 sRGB
TF、不覆盖 gamut**,且会改变现有混合行为 → 仅作未来质量旋钮,不进默认路径。

### 2.3 转换模块:自建 mini-skcms,不 vendor skcms(v2 定稿)

新建 `src/graphic/color_xform.{hpp,cc}`(命名实施期可调),内容:

1. **`ColorXformSteps`**:src→dst 变换的分解结果 `{linearize?, gamut matrix?,
   encode?}`,`Resolve(src, dst)` 产出并做全部快路径折叠(同空间→noop;
   src 线性→省 linearize;dst 线性→省 encode;gamut 相等→省矩阵;dst TF ==
   sRGB 曲线→encode 用编译期常量曲线而非 uniform 参数)。CPU `Apply`(unpremul
   域,alpha 透传,RGB 不截断);
2. **CPU buffer kernel**:`TransformPixels(src Pixmap, dst Pixmap, steps)`
   (premul 进出、量化),服务 ReadPixels/snapshot/codec 编码边界;
3. **WGSL 生成**:把 steps 发射为 shader 片段(泛化现
   `WriteColorSpaceDecode` + `cs_srgb_encode_channel`),供纹理采样解码与
   layer 合成共用。

不 vendor skcms 的理由:输出转换只需要参数化数学(值类型已具备);skcms 主体
价值是 ICC 解析/CLUT/SIMD buffer transform,当前都不需要;Skia 自己的 shader 侧
steps 分解(`SkColorSpaceXformSteps`)也不是 skcms 提供的。**再议判据**:WebP/JPEG
ICC 支持提上日程时,重评 vendor skcms(获得其 ICC 解析器),值类型桥接届时设计。

### 2.4 GPU 注入点(v2 泛化)

- 纹理采样解码:`WGSLTextureFragment` 现有解码块从"→ sRGB"泛化为
  "→ 目标空间"(steps uniform 驱动,零 shader 变体原则不变;dst TF 为 sRGB
  曲线时保持现有常量曲线写法,字节不变);
- **不存在 per-draw 输出编码步骤**:目标空间即工作空间,fragment 输出直接存储;
  转换只在跨空间边界出现——layer 合成(子 layer space ≠ 父目标时,layer 绘制
  的纹理着色器里插 steps 块,复用同一 WGSL 生成);
- 目标空间的管线传递:沿 `HWDraw::SetColorFormat` 现有链路加
  `SetColorSpace`(surface → root layer → sublayer/filter layer,
  `hw_layer.cc:111,200,429`、`hw_canvas.cc` GenLayer 处)。

### 2.5 CPU 转换边界(v2 调整)

- **Paint/DisplayList 颜色携带空间,draw 时解析**(替代 v1 设色时机转换):
  `Paint` 持有 `{Color4f, ColorSpace}` 值成员(默认 sRGB 零成本);`SetColor4f`
  原样记录,`GetColor4f()` 返回记录值、新增 `GetColorSpace()`;转换发生在
  HWCanvas/SWCanvas 派发绘制、填 uniform/渐变 stop 数组时(→ 目标空间);
- **SW 采样**:`BitmapSampler::GetColor()` 出口转换泛化为 → bitmap 空间;
- **SW 合成**:SWCanvas 渲染进用户 Bitmap,颜色在 span 生成时转换到 bitmap 空间
  (与 GPU 语义一致:混合留在目标编码空间);
- **回读**:surface/snapshot 回读的 Pixmap 打**目标空间**标签(如实反映存储
  字节);跨空间导出走 `TransformPixels` 显式转换。

### 2.6 呈现与 swapchain(机制预留,一期不实施)

事实:swapchain colorSpace 是**声明而非转换器**——合成器按声明解释存储字节,
不做任何转换;像素必须已是该空间编码。各平台:

| 平台 | 能力 |
|---|---|
| Vulkan | `imageColorSpace`:`SRGB_NONLINEAR`;`DISPLAY_P3_NONLINEAR_EXT` 等需 `VK_EXT_swapchain_colorspace`;`gpu_presenter_vk.cc:535` 已做格式选择,扩展为按请求空间选 colorSpace |
| Metal | drawable 格式 + layer 配置,系统合成器解释,细节实施期确认 |
| WebGPU | `GPUCanvasConfiguration.colorSpace`(`srgb`/`display-p3`) |
| GL | 窗口层面基本无(仅 `EGL_KHR_gl_colorspace`,sRGB/linear 两值且平台参差);纹理层面仅 `SRGB8_ALPHA8`;skity GL surface 是用户 FBO/纹理形态,输出空间 = 声明的像素空间,不受影响 |

策略:目标空间机制天然兼容 present(对齐即直写);接 swapchain colorSpace 是
呈现层的参数化工作,不改架构,排三期。

### 2.7 滤镜与图像滤镜的空间语义(v2 定稿,RGBA8 编码空间下)

转换清单(全部转换,无遗漏):图像采样解码(GPU 逐像素,仅空间≠目标时)、
Paint色/渐变stop/`Blend`滤镜色(CPU 逐 draw,uniform 填充时)、layer 合成
(GPU 逐像素,仅跨空间时);**不存在输出编码步骤**;ReadPixels/codec 导出按需。
推论:sRGB 目标与 v1 提交逐字节一致;P3 图 → P3 surface 零逐像素转换零裁剪。

Color filter(四类,管线位置 `sample → decode → filter_color() → blend` 不变,
滤镜永远看到目标空间值):

- `LinearToSRGBGamma`/`SRGBToLinearGamma`:TF-only。**Display P3 与 sRGB 共享
  TF 曲线,P3 目标上语义与代码均零改动**(实际做 P3-encoded↔P3-linear)。目标
  TF ≠ sRGB 曲线的 parametric 空间上按字面 sRGB 曲线处理(与 Skia
  `SkGammaColorFilter` 同款语义,不隐式跟随目标空间);
- `Matrix(4x5)`:语义 = 作用在**目标编码空间**的值上,不做隐式转换(可预测 +
  零回归;为 sRGB 调的矩阵在 P3 目标结果不同,由使用者负责);
- `Blend(color, mode)`:颜色参数随 Paint 路径 draw 时解析,零改动;
- `Compose`:随内层。CPU `FilterColor(PMColor)` 同样在目标空间值上运行。

Blur/图像滤镜:**数学 space-agnostic,代码零改动**(premul 加权卷积与空间无关)。
语义 = 在 layer 空间编码值上卷积,与现状 sRGB 行为同类;**P3 的 TF == sRGB TF,
P3 目标上 blur 误差轮廓与今天完全相同**。空间归属:blur 输入永远是单一空间的
layer 纹理(解码在 draw 阶段已完成),跨空间转换包裹在**过滤结果之外**的合成
步骤里,顺序无歧义。中间纹理沿用 layer 格式(8888),精度不变。远期质量旋钮:
filter 中间层单独 fp16(局部改动,不依赖全局线性模式)。

### 2.8 明确不做项(v2)

- 不做:ICC/CLUT(§6)、vendor skcms(再议判据见 2.3)、渐变 stop 序列化空间
  (`kHasColorSpace_GSF` 占位保留)、surface 级**渲染意图**选项(只有无裁剪的
  colorimetric 语义)、HDR/PQ 传递函数(类型上不堵死);
- 一期不做:fp16 线性目标模式、on-screen present 空间声明、C API surface 空间
  参数(pixmap POD 通道已就绪)。

## 3. 公共 API 增量(相对 v1)

`include/skity/graphic/color_space.hpp` 不变(可能补 `DisplayP3Linear()`
工厂随二期)。新增/调整:

```cpp
// 目标空间入口(v2)
struct GPUSurfaceRenderOptions { ...; ColorSpace color_space; };  // 默认 sRGB
// snapshot:GPUContextImpl::CreateRenderTarget/MakeSnapshot 继承或指定空间
// GPUSurface::ReadPixels:返回 Pixmap 打 surface 空间标签(转换由显式
//   TransformPixels/后续参数完成)

// Paint(v2 调整)
void SetColor4f(Color4f c, ColorSpace cs = {});  // 原样记录,不再设色时转换
Color4f GetColor4f() const;                       // 返回记录值
ColorSpace GetColorSpace() const;                 // 新增

// 转换模块(内部为主,少量公开)
// src/graphic/color_xform.hpp:ColorXformSteps / TransformPixels
```

## 4. 转换数学与色域原理

### 4.1 分解模型:颜色空间 = TF + 色域

一个 RGB 颜色空间是两层编码的叠加,对应 `ColorSpace` 的 16 个 float:

```
存储信号值 (0-255 字节)
  │ ① 传递函数 TF(一维非线性曲线,逐通道独立)
  ▼
线性 RGB(光强比例)
  │ ② 色域矩阵 3×3(线性变换,三通道耦合)
  ▼
CIE XYZ(与设备无关的绝对坐标)
```

**色域(gamut)即第②层:RGB 三原色在 CIE 1931 色度图上圈出的三角形**,
回答"这套 RGB 坐标能表示哪些真实颜色"。色度坐标由 XYZ 投影去亮度:
`x = X/(X+Y+Z)`,`y = Y/(X+Y+Z)`。sRGB 与 P3 的原色(色度坐标):

| 原色 | sRGB | Display P3 |
|---|---|---|
| 红 | (0.640, 0.330) | (0.680, 0.320) 更靠光谱轨迹、更饱和 |
| 绿 | (0.300, 0.600) | (0.265, 0.695) |
| 蓝 | (0.150, 0.060) | (0.150, 0.060) |

P3 三角形基本包含 sRGB —— 宽色域的含义即在此。

### 4.2 色域矩阵的推导(原色 + 白点 → RGB→XYZ)

设原色色度 $(x_i, y_i, z_i)$($z_i = 1-x_i-y_i$)、原色亮度 $Y_i$:

```
M = [ x_r/y_r  x_g/y_g  x_b/y_b ]   [ Y_r 0   0  ]
    [ 1        1        1       ] · [ 0   Y_g 0  ]
    [ z_r/y_r  z_g/y_g  z_b/y_b ]   [ 0   0   Y_b]
```

$Y_i$ 由白点条件解出:`M·(1,1,1)ᵀ = 白点 XYZ`(D65 ≈ (0.9505, 1, 1.089))。
由此得到校验性质 **M 每行元素之和 = 白点**(单测
`DisplayP3GamutMatchesPrimariesAndWhitePoint` 验证;`color_space.cc` 的
kDisplayP3Gamut 即如此预计算)。矩阵的**列**是原色的 XYZ,矩阵张成的锥体
就是色域三角形。

### 4.3 色域转换

颜色必须经过 XYZ 中转(格拉斯曼定律:颜色混合线性性只对**线性光强**成立,
在 gamma 编码值上做矩阵乘法无物理意义):

```
signal_src --TF_src⁻¹--> linear_src --M_dst⁻¹·M_src--> linear_dst --TF_dst--> signal_dst
```

- $rgb_{dst} = M_{dst}^{-1} M_{src} \cdot rgb_{src}^{linear}$;steps 解析期用
  伴随矩阵法求逆并预组合为单个矩阵(`ColorXformSteps::Resolve`);
- 快路径折叠:同空间 → noop;src 线性 → 省解码;dst 线性 → 省编码;
  gamut 位相同 → 省矩阵;dst TF == sRGB 曲线 → WGSL 用编译期常量曲线;
- **越界值不截断**:P3 红线性 (1,0,0) 经 $M_{sRGB}^{-1}M_{P3}$ ≈
  (1.093, −0.227, −0.150),负值的几何含义是该原色点在 sRGB 三角形外。
  `Convert/Apply` 按契约保留(信息留给上层 gamut mapping 决策),仅量化
  写 8bit 时 clamp(golden `P3TargetAlphaCompositingAndReadbackRoundTrip`
  的 1.037 断言锚定此语义)。

### 4.4 传递函数

7 参数分段形式(skcms 布局 `{g,a,b,c,d,e,f}`):

```
linear(x) = c·x + f            (x <  d)
linear(x) = (a·x + b)^g + e    (x ≥ d)
```

sRGB 取值 `g=2.4, a=1/1.055, b=0.055/1.055, c=1/12.92, d=0.04045`,即
暗部 `x/12.92`、亮部 `((x+0.055)/1.055)^2.4`。分段原因:纯幂在 0 附近
斜率无穷大,暗部 8bit 精度极差,线性段接住(两端斜率近似连续)。gamma 2.2
空间是退化单段形式(d=0)。

**闭式求逆**(该参数族对求逆封闭,无需 Newton 迭代):由
$y=(ax+b)^g+e$ 反解 $x=((y-e)^{1/g}-b)/a$,代码把 $a^{-1}$ 吸收进幂底数
整理回同族形式:

```
inv = { 1/g, a^{-g}, -e·a^{-g}, 1/c, c·d+f, -b/a, -f/c }
```

其中 `d' = c·d+f` 是分段边界翻转。sRGB 验证:
$(a'·y)^{g'} + e' = 1.055·y^{1/2.4} − 0.055$ ✓(单测 `Apply(0.21404114) ≈ 0.5`)。

### 4.5 premul 顺序

内部像素为预乘格式,转换必须先除 α、转换 RGB、再乘回 α——premul 把 α 混进了
RGB 分量,直接变换等于变换被缩放的坐标。WGSL 解码块中 `cs_rgb / color.w` 与
`cs_rgb * color.w` 那对包裹、`TransformPixels` 的 `need_unpremul/need_premul`
分支即此事;noop 转换跳过这对除乘,因为该往返不字节精确(127/255 往返可差 1)。

### 4.6 白点约定(D50 连接空间,评审后修正)

**全部 gamut 矩阵统一 Bradford 适配到 D50 连接空间**(CIE D50 XYZ =
(0.96422, 1, 0.82521),与 ICC profile colorants 同约定)。sRGB 常量本就是
ICC 的 D50 适配形态(行和 = 0.96428, 0.99997, 0.82509,单测反推标准原色
(0.640,0.33)/(0.3,0.6)/(0.15,0.06) 独立证实);P3 常量按标准原色
(绿 y=0.690)D65 原生矩阵 Bradford→D50 重推导(0.5151021, 0.2919648,
0.1571531 / 0.2411818, 0.6922363, 0.0665819 / -0.0010494, 0.0418818,
0.7843776);PNG cHRM 读侧把文件白点适配到 D50,写侧反适配到 D65 再取原色
(x+y==1 边界原色留 1e-4 epsilon)。早期版本 §4.6 曾写"全部矩阵 D65、互转
免色适应"——当时没发现 sRGB 常量实际是 D50 的,评审 #1 指出混用使 P3↔sRGB
全部带系统偏差(P3 中性灰 0.5 → (0.458, 0.505, 0.578))。适配相消时残差
来自 sRGB ICC 定点舍入:中性灰/白 ≤ 1.3e-4(远小于 1/255),线性 P3→sRGB
矩阵与公开值差 ≤ 1.76e-4。独立参考实现见
`test/ut/graphic/color_space_test_util.hpp`(双精度,不依赖被测代码)。

## 5. 集成点一览(v2)

| 关注点 | 插入点 | 状态 |
|---|---|---|
| 转换模块 | `src/graphic/color_xform.{hpp,cc}`:steps 分解 + buffer kernel + WGSL 生成 | v2 新建(数学从 color_space.cc 迁入/复用) |
| GPU 采样解码 | `wgsl_texture_fragment.cc` 解码块泛化 → 目标空间 | v1 已有,泛化 |
| 目标空间传递 | surface/render options → `HWDraw::SetColorSpace` → layer 链 | v2 新增 |
| layer 合成 | 子 layer space ≠ 父目标时纹理绘制插 steps 块 | v2 新增(复用 WGSL 生成) |
| Paint/渐变 | Paint 携带空间;draw 派发时解析到目标空间 | v2 调整(去掉 set-time 转换) |
| SW 采样/合成 | sampler 出口 → bitmap 空间;span 颜色 → bitmap 空间 | v1 已有,泛化 |
| snapshot/ReadPixels | 打目标空间标签;`TransformPixels` 显式导出 | v2 新增/调整 |
| codec | PNG gAMA/cHRM 读写在 v1 已有;现在真正接上输出流 | v1 已有 |
| C ABI | `skity_color_space_info` POD + pixmap 通道已有;surface 参数二期 | v1 已有 |
| 呈现 | Vulkan colorSpace 选择、WebGPU canvas colorSpace、MTL 确认 | 三期 |

## 6. ICC 演进定位(不变,补充 v2 结论)

值类型不堵死 ICC;将来 `FromICCProfile(const void*, size_t)` 解析归约到同一
16-float 结构。**v2 补充**:届时重评 vendor skcms(其 ICC 解析器与 SIMD
transform 直接可用),桥接方式 = ColorSpace 值成员之外挂 profile 引用或
`kICCDerived` kind + 解析期归约,不改变转换模块的 steps 接口。

## 7. 测试计划(v2 增量)

- 单测:`ColorXformSteps::Resolve` 折叠正确性(同空间 noop/线性省略/常量曲线
  掺杂);`TransformPixels` premul 往返、量化;P3 surface 下 Paint/渐变 draw 时
  解析;layer 跨空间合成;
- golden:P3 surface + P3 标记图像 → 输出 P3 编码字节(P3 红 `255,0,0` 不被
  sRGB 往返);P3 surface → sRGB 导出转换;零回归:全默认 sRGB 与基线逐字节一致;
- 验证命令与风格门禁沿用 v1(全部经 `tools/test-runner.py`,构建目录
  `out/cmake_host_build`)。

### 7.1 golden 形态决策(定稿)

色彩空间 golden 用例采用**自包含 oracle 形态**(RenderToTexture 走完整 GPU
链路 → ReadPixels 原始字节回读 → 与 CPU Convert 期望比对),不用基准图;
`DisplayP3AndLinearSRGBDecode` 保留一份 v1 基线作混合覆盖。三层防线:
单测硬编码物理锚点(D65/原色色度/0.21404114 等)抓共享数学漂移;oracle
golden 抓 GPU≡CPU 不一致与管线接线缺失(paint 解析 bug 即此类抓到);
golden 内绝对断言(字节精确 255,0,0)锚物理值。ReadPixels 按设计零转换
(打标签不转换),回读侧无隐藏变换,oracle 比较是裸比较。跨空间导出统一
走显式 TransformPixels。

## 8. 实施顺序(v2)

1. **转换模块**:`color_xform` steps + kernel + WGSL 生成 + 单测(纯增量,不动管线);
2. **目标空间管线**:surface/layer/HWDraw 传递;采样解码与 SW 泛化;
   Paint/渐变 draw 时解析(删除 set-time 转换);
3. **输出边界**:snapshot/ReadPixels 语义、codec 编码 e2e、golden 用例;
4. **模式与呈现**(后续 PR):fp16 线性目标;Vulkan/WebGPU present 空间声明;
   C API surface 参数;`DisplayP3Linear()`。

**实施状态(2026-11,Phase 1-3 已落地并全量验证)**:

- Phase 1 ✅:`src/graphic/color_xform.{hpp,cc}`(steps 分解/Apply/
  TransformPixels);`ColorSpace::Convert` 委托到 steps(单一数学实现);
  TF 求值/求逆导出到 color_space_priv;
- Phase 2 ✅:`GPUSurfaceRenderOptions.color_space` → `GPUSurfaceImpl` →
  `HWDrawContext.target_space`(sub_context 拷贝透传,saveLayer 自动继承);
  `WGSLTextureFragment` steps 化(uniform 驱动 `cs_flags/cs_tf*/cs_gamut/
  cs_enc_tf*`,零新增 shader 变体,dst 为 sRGB 曲线时保持常量曲线写法);
  Paint 携带空间 + `GetColor4f(target)`(旧式 SetColor 系列重置为 sRGB);
  rrect/text 几何、GPU/SW 渐变 stop、SW solid brush 全部 draw 时解析;
  `BitmapSampler` 采样出口 → 目标空间;
- Phase 3 ✅:`MakeHWImage(texture, space)` / `DeferredTextureImage::
  SetColorSpace`;`HWDraw::SetColorSpace` 链(root layer 于 BeginNewFrame、
  子 layer 于 GenLayer 注入 surface 空间),layer 合成 shader 与 MSAA
  resolve 路径按目标空间标记(同空间直通零转换);golden env
  `SetTargetColorSpace`(MTL/GL/VK)+ P3 直通/Paint golden 用例;
- 验证:unit 1096/1096、golden-shape 275/275、golden-text 11/11、
  cpplint + clang-format 通过。golden 色彩空间用例共 7 个,覆盖:
  sRGB 目标解码(v1)、P3 目标图像直通/paint 解析、线性目标(encode 折叠
  GPU 分支)、gamma-2.2 目标(参数化 encode uniform 分支)、P3 saveLayer
  合成(layer 纹理空间标记)、P3 渐变 stop 解析(kClamp 端点消除插值约定
  耦合)、P3 半透明合成 + 回读 `TransformPixels` 闭环(含超出色域量化截断
  断言);另有 SW e2e 单测(P3 bitmap 目标:solid 解析 + 图像直通);
- 追加(`b3cc9605`):内置 `ColorSpace::AdobeRGB()`(gamma 2.2 + Adobe D65
  gamut,常量由原色+白点约束推导,单测锚定行和/列比例);example 增
  AdobeRGB 标签行/paint 块/三原色绿对比;golden 增 AdobeRGB 解码与
  目标直通两用例(unit 1101/golden 279);
- 追加(`f161e46b`):渐变 stop 声明空间(`Shader::SetColorSpace`,
  全部 stop 共用一个空间,工厂签名不变)+ `ColorFilters::Blend` 的
  Color4f/color_space 重载(应用时按目标空间解析,CPU 路径按 target
  缓存);渐变序列化启用 `kHasColorSpace_GSF`(16 float),C ABI 增
  `skity_shader_set_color_space`;新增 3 单测 + C ABI 冒烟 + golden
  `P3DeclaredStopsResolveIntoSRGBTarget`(unit 1100/277/11);
- 追加(`3f985337`):WebGPU harness 色彩空间用例 ×7(wasm + SwiftShader,
  真实 WGSL 无交叉编译路径,独立于 MTL/GL golden 的 tint 链路)。
  `GeometryCase` 增 `target_space` 字段 → `GPUSurfaceDescriptorWEB.
  render_options.color_space`(P3/线性/gamma-2.2 目标);覆盖 P3 目标图像
  直通、paint 双向解析(sRGB 录制→P3 解析 + P3 录制直通)、线性目标
  encode 折叠、gamma-2.2 参数化 encode、渐变 stop 双向(sRGB stop→P3
  目标;P3 声明 stop→sRGB 目标)、Blend 过滤器绑定组解析。基线像素经
  独立 CPU 数学(decode→gamut M_dst⁻¹·M_src→encode)校验 ±1 byte,
  白点核验 (255,252,224)、sRGB red→P3 (240,49,28);web-harness 18/18;
- **PR 拆分(2026-11)**:#555 缩回 v1 范围(codec 标签 + GPU 解码);
  v2 按 #570(核心管线:目标空间工作模型 + color_xform + paint 解析)
  → #571(example + Metal layer sRGB 声明修复)→ #572(渐变/blend
  声明空间 + AdobeRGB + web 用例)堆叠拆出。拆分时把
  color_space_display_p3.png 从 3b5af5bf 历史中彻底剔除(amend);
  #572 树与拆分前 tip 逐字节一致(diff 空)。本地备份分支
  `backup-color-space-full`(2a7cdec4,未推远端)。B 树本地验证
  unit 1096/golden 276/11,C 树 example 编译通过;注意 out/cmake_host_build
  需重新 configure 才能看到新增的 example target;
- **合入流程(squash 仓库的堆叠 PR)**:顺序严格为 #555 → #570 → #571
  → #572。main 是线性 squash 历史,原 commit 不会成为 main 的祖先,
  所以前一个合入后,后一个 PR 的 diff **不会自动缩小**,必须逐级
  `rebase --onto` 丢掉已合入的 commit 再 force push:
  `git fetch origin && git rebase --onto origin/main <旧父tip> <分支>`,
  旧父 tip 依次为 `5ea1600f`(→ core)、`89370236`(→ example)、
  `4617f270`(→ stops);下一级分支里保留的是原始 commit,所以每次
  rebase 的"旧父 tip"都用这里记录的原始 SHA;
- **#555 commit 重写(2026-11)**:2dbbfd4e 按依赖重写为 3 个 commit——
  b9f72382(类型与数学,含 color_filter 常量去重 + 前 12 个纯类型单测)
  → 26618da4(标记与解码:pixmap/image/paint + GPU/SW 解码 + golden/
  web 用例,测试文件后 3 个标记 API 测试在此补齐)→ e1f6dffd(codec
  gAMA/cHRM + C ABI)+ 590b840d(style)。tip 树与旧 5ea1600f 逐字节
  一致(diff 空)。中间树逐级本地验证:A1 12/12、A2 unit 48/golden 2。
  注意:test-runner 的 --filter 透传 gtest 语法,`ColorSpace` 匹配 0 个,
  要写 `ColorSpace.*`。下游 #570/#571/#572 分支未动,其 rebase 记录的
  旧父 tip SHA 仍有效。另发现:`test/ut/graphic/bitmap_sampler_test.cc`
  填了 56 行测试但从未挂进任何 CMake/gn(空占位文件的遗留),一直没被
  编译——已补挂进 #555 的第二个 commit(254c1983,tip e388746d),
  首次编译并跑通 2/2;下游分支 rebase 时该行由 3-way merge 自动带过;
  重写后首轮 CI 的 `android aar size` 卡死在 Java Setup 阶段(无构建
  步骤执行),为 runner 基础设施问题,重跑即可;
- **D50 统一(评审 #1/#6,#555 commit `9df46641`)**:见 §4.6。顺带修正
  P3 绿原色 0.695→0.690;新增 4 个独立锚点测试(SRGB/P3 反推原色、中性灰
  双向、线性 P3→sRGB 公开矩阵、cHRM 反向函数);单测 1089、golden 268/11、
  web decode 基线不变(同矩阵往返代数相消)。**下游待办**:#572 AdobeRGB
  常量需同样 D50 化(D65 原生推导);#570 的白点怪癖断言(白→P3 =
  (255,252,224))会变为 (255,255,255),P3TargetAlphaCompositing 期望与
  #572 web P3 基线需重生成+CPU 校验;example 三原色对比行数值变化;
- **#570 整体 shared_ptr 适配 + 重建(`1379c8b9`)**:分支重建为
  `9df46641`(#555 新基)+ 单一 squashed commit(v2 核心+paint 修复+显式
  空间 API 合并);12 个交叠文件走三方合并(方向:v2 语义+shared_ptr 形态),
  27 个 v2 独有文件取 tip 后适配。`ColorXformSteps::Resolve(sp,sp)` null→
  `*ColorSpace::SRGB()` 单例解引用(数值相等语义保留);Paint API 全部
  **双 overload 化**(`SetColor(Color)` + `SetColor(Color, sp)`,四个 setter
  一致,不用默认参数——用户明确要求两个重载);golden env
  `SetTargetColorSpace(sp)`;树验证 unit 1103/golden 276/11 全绿。
  **适配踩坑记录**:paint.cc 单参 SetColor 定义早已删除,方向应是新增;
  正则批量改断言容易把 EXPECT_TRUE/FALSE 逻辑改反(两轮手工修复);
  null 解引用崩溃(plain setter 记录 nullptr)需统一
  `ColorSpaceIsSRGB(sp)`;merge-file 残留多余 `}` 三处;
- **Paint 空间显式化(#570,已并入上面 squash)**:删除 `Paint::SetColorSpace`
  粘滞 setter;`SetColor/SetStrokeColor/SetFillColor` 增加 ColorSpace 参数
  重载(无参形式显式记录默认工作空间 sRGB)。**stroke/fill 各持空间**
  (`stroke_color_space_`/`fill_color_space_` 成员),消除"设 fill 后设
  stroke 会重置共享空间"的二层二义性;`GetColorSpace()` 返回按 style 选中的
  那个,新增 `GetStrokeColorSpace()/GetFillColorSpace()`;glyph 拷贝改为
  `SetColor4f(color, 源paint对应通道空间)`(float 精度,不走 8-bit 量化);
  回归测试 `PaintColorSpaceIsExplicitPerColorSet`。注意:#571/#572 分支仍带
  旧 #570 基底,适配时继承;下游 shared_ptr 化时这些签名一并改
  `std::shared_ptr<ColorSpace>`/nullptr 默认;
- **shared_ptr 改造(2026-11,#555,tip `54c3b55b`)**:与同事讨论后把
  ColorSpace 从值类型改为引用语义——公开 API 直接传
  `std::shared_ptr<ColorSpace>`(与 Shader/ColorFilter/Image 习惯一致),
  `nullptr` = 未声明 = sRGB 工作空间。`ColorSpace` 本身仍是公开数据
  struct(16 float + kind,序列化/C ABI 镜像零变化)。命名工厂返回
  函数局部静态单例(interning,指针快路径);`MakeRGB` 非法参数返回
  nullptr(原 optional);`Convert/ConvertColors` 改为 null 容忍静态方法
  `ColorSpace::Convert(sp, c, sp)`;新增头文件内联
  `ColorSpaceIsSRGB(sp)` / `ColorSpaceEquals(sp,sp)`(后者 null≈sRGB)。
  实例 `operator==` 保持数值相等忽略 kind(PNG round-trip 依赖)。
  语义变化:**"默认 sRGB"现在可观测为 null**——PNG 无色彩 chunk/
  sRGB chunk 时 decode 不打标,`GetColorSpace()` 返回 null(旧值语义
  返回 sRGB 值);C ABI getter 对 null 物化规范 sRGB POD,消费方必须
  用 ColorSpaceIsSRGB 而非裸解引用(适配时踩过:EncodeDecodeDefaults
  段错误)。#555 树全绿:unit 1085 / golden 268/11,门禁过,commit 1/2
  中间树逐级验证(12/12、18+2)。**下游 #570/#571/#572 未动**(仍带
  旧 v1 值 API 基底),后续分步适配清单:core(color_xform Resolve、
  surface options、paint/hw threading)→ example 调用点 → stops
  (shader/blend/C ABI/web);注意 `*MakeRGB(...)` 解引用optional 的
  用法(Gamma22TargetSpace)要改;本地回退 ref:`backup-555-v1`
  (e388746d 值 API 版);
- 明确剩余:渐变插值空间(对应 Skia Interpolation,预组合 stop 变换
  可实现)、`ColorFilters::Blend` 空间的序列化(现 flatten 转回 sRGB
  保显示色真)、顶点颜色空间、snapshot 公开 API 的空间参数(现固定
  sRGB target)、WebP/JPEG ICC、第 4 期全部内容。

## 9. 后续演进(显式不在本期)

- `FromICCProfile()` 与 vendor skcms 重评(§6);
- PNG cICP chunk 与编码侧 sRGB chunk 写回;
- 渐变 stop 颜色空间 / 线性空间插值(接 `kHasColorSpace_GSF` 占位);
- fp16 线性目标模式全后端(含 SW 浮点缓冲评估);
- sRGB 硬件附件格式(线性混合质量旋钮,改变混合行为需独立开关);
- on-screen HDR(PQ)与 float swapchain;
- 对外 C 消费者的 ColorSpace 域。

## 10. 二轮 review 修复记录(deace3de → 16a9a91f,2026-11-11)

外部二轮 review(`ColorSpace-review-deace3de`)11 项全部核实属实,10 项在本轮修复
(#5 const 化除外,等与同事确认 API 形态)。分组落地 4 个 commit:

1. **`dc8dc303` PNG 解码重构(#1/#2/#10)**
   - #1 gAMA 方向:chunk 存的是编码 gamma(45455=1/2.2),内部 tf 是解码指数
     (signal→linear),读写各取一次倒数。往返断言放宽到 1e-4:45455 定点量化
     经浮点倒数恰为 2.1999781(差 2.2e-5),不是 bug。
   - #2 双重转换:libpng 简化 API(`png_image_finish_read`+`PNG_FORMAT_RGBA`)
     的契约是输出 sRGB 编码字节(它按文件 gamma 做了转换),再附源曲线标签
     = 二次转换。`DecodeIntrinsic` 改低层 API:不设 gamma 变换拿源编码字节,
     转换只在采样时发生一次(与 GPU decode 架构一致)。palette/灰度/16bit/
     tRNS/filler/interlace 的规范化变换链齐了。
   - #10 坏 CRC:删自有 chunk 扫描器,元数据从 libpng info 读
     (png_get_gAMA_fixed/png_get_cHRM_fixed)。png.h 明确 ancillary chunk
     CRC 错默认 warn/discard —— 元数据与像素同一套校验规则。
   - 踩坑:测试里手写的 1x1 PNG 在 IDAT CRC 和 IEND 之间有 4 个游离字节
     (75 CA 82 5F),简化 API 默默容忍、低层 API 正确拒绝 → 修测试资产。
   - 回归测试全部按规范真值而非自往返:写方向断言 45455;读方向断言源字节
     保留(gamma PNG 128→显示 129、linear PNG 64→显示 137);坏 CRC 忽略。

2. **`e54e58a2` CPU 采样(#3/#4)**
   - #3:NEON 最近邻块直接读原始像素,只在余数像素走 sampler → 每 8 像素
     一条接缝。非 sRGB 标签时整段回退标量(照 kLinear 分支先例)。
     验证:stash 撤销后 9 像素 span 前 8 个 64、第 9 个 137(复现值一致)。
     注意 `SKITY_ARM_NEON` 在 macros.hpp 由 `__aarch64__` 自动定义,
     本机 arm64 就走 NEON 路径,本地可测。
   - #4:预乘采样直接进非线性曲线(64/128 → 137 > alpha)。sampler 记录
     AlphaType,premul 时 unpremul→转换→re-premul,alpha=0 保持精确 0
     (≈94)。#570 rebase 后该逻辑与 steps_.Apply 合并共存。

3. **`e74095b5` GPU/缩放(#7/#8/#9)**
   - #7:重采样 dst 用 MakePixmapAlloc 建丢了标签 → 传播 `src->GetColorSpace()`。
   - #8:`ScalePixelsForTexture` 只收 texture,`MakeHWImage(texture)` 默认
     sRGB。MakeHWImage 改两参(带默认 nullptr,与 #570 签名完全一致,rebase
     零冲突),ScalePixels 传 color_space_。golden 用例复现 64→137。
   - #9:WGSL re-premul 也在 `color.w > 0.0` guard 里,非零黑点曲线的
     透明黑泄漏 (89,89,89)。除法保留 guard,乘法无条件(乘 0 恰好保证
     预乘输出 RGB=0)。shader 期望串测试同步更新(注释也在生成源码里!)。

4. **`1bcbe04c` 纯 gamma 负分量(#6)**
   - {g,1,0,0,0,0,0} 的 c=f=0,负分量进线性段归零;P3 红往返变
     (1.0033,0.2227,0.1556)。修复:退化线性段对负输入镜像 power 分支
     (sign-preserving),曲线保持可逆;逆曲线同为纯 gamma 形式自动受益。
     CPU `ColorSpaceEvalTransferFn` 与 WGSL `cs_eval_tf_channel` 同规则
     (#570 rebase 时 git rename 检测把 WGSL 修复自动带到了 v2 helper)。

5. **`16a9a91f` ConvertColors 批内 hoist(#11 的 #555 部分)**
   - PreparedConversion(解码曲线/组合矩阵/编码曲线)批外准备一次,循环内
     只做颜色数学(reviewer 测量 ~35% 批量时间在准备)。
   - **#570(7d0c6754)上该实现被 ColorXformSteps 取代**(Resolve 一次 +
     ApplyColors 循环,sampler 构造时备好 steps_)—— review 回复时说明
     sampler 级缓存在 stacked PR。

**#570 rebase(84b7413c → 7d0c6754)**:7 个冲突文件;color_space.cc 取 v2
(ColorXformSteps 委托)但镜像分支加进导出的 ColorSpaceEvalTransferFn;
bitmap_sampler 合并双方(steps_ + premul);wgsl 取 v2 cs_flags 结构再叠
#9 guard 修复;golden 测试两侧新增都保留。unit 1111/golden 277/门禁过。

**剩余未修**:#5 单例可变(shared_ptr<const ColorSpace> 全线改造,涉及所有
公开签名与下游三分支,等与同事确认)。二轮 review 11 项中 10 项闭环,
#5 是唯一挂起项。

### 10.1 遗漏复查补录(2026-11-11 晚)

对照 review 文档逐项复查发现的遗漏,均已补:

- **web-harness 修复后未重跑** → 补跑:#555 11/11、#570 11/11 全绿。
  web 的 7 个 P3 target 用例不在 #570,在 #572(ba819023),不是 rebase
  丢失;#572 适配时基线要按 #6 镜像/#9 guard 重新验证(渐变 stop
  中间可能出现负分量)。
- **perf 未复测** → Release 库 + 独立 bench:1M 色 P3→sRGB,
  convert-per-color(修复前批量形态)中位 62.94ms,ConvertColors
  (prepared)30.14ms,-52%,两路径输出逐位一致(唯一 1 个 mismatch 是
  bench 自身 warmup 污染 index 0)。
- reviewer 原样 unit 过滤命令 45/45;metal backend ColorSpaceGolden 3/3。
- 边界确认:#8 仅 TextureImage/PixmapImage 两个 ScalePixels override
  (后者走带标签 pixmap);#9 的 WriteColorSpaceDecode 单一生成点,
  普通+cubic 共用;#3 的 bilinear/cubic 在构造时归一 kLinear 已回退。
- review 回复要点:#5 挂起待 API 确认;#11 sampler 级缓存在 #570;
  reviewer 诊断用例名 ↔ 我们的等价测试映射表。

### 10.2 #5 const 化落地(方案A,2026-11-11)

用户确认走 reviewer 建议的方案 A:`shared_ptr<const ColorSpace>` 全线。

- **#555(b191974d,14 文件)**:类型层(工厂 4 个 + Convert/ConvertColors +
  ColorSpaceIsSRGB/Equals)→ 持有方(Pixmap/Image/Paint/BitmapSampler/
  WGSL fragment/C ABI ColorSpaceFromCInfo)→ 测试/harness。工厂内部
  lambda 先造非 const 再存 const 静态(make_shared 隐式转换)。补
  `NamedSpacesAreImmutable` 测试:static_assert 钉死工厂返回类型 +
  字段不可赋值(编译期保证,reviewer 的运行时复现用例不再可编译)。
  行为零变化:unit 1098/golden 269/web 11 全绿。
- **#570(71de3d05)**:rebase 冲突 5 文件(v2 结构 + const 签名合并),
  另 16 个 v2 独有文件机械替换(color_xform、gpu_surface、hw_draw、
  sw_span_brush、golden_test_env 等)。`Resolve` 的
  `src ? *src : *ColorSpace::SRGB()` 解引用单例的路径随单例不可变而
  封闭 —— null 与单例两种 sRGB 表达不可再分裂。unit 1112/golden/web 绿。
- `std::move` 传 const-ref 参数的两处(Convert 委托/ConvertColors)去掉。
- PR 555/570 描述措辞更新(immutable shared_ptr<const>)。
- **#571/#572 适配时**:同款机械替换;#572 的 `*MakeRGB(...)` 解引用
  用法(Gamma22TargetSpace)随 const 化要改读取方式(本来就是读,解
  引用 const 对象读字段合法,无需改;只有写才受限 —— 实际可能零改)。

## 11. #555 拆分合入(2026-11-12)

用户判断"一次改动太多容易引入问题"(二轮 review 11 项佐证),决定把 #555
拆成 4 个 PR 逐步合入;#555 转 draft 作 umbrella。

**新链**(merge 顺序即此序,每次 squash-merge 后其余 rebase --onto):
1. #575 feat-color-space-type — 类型+数学(color_space.hpp/cc/priv、
   color_filter wire、类型测试 17 个、cpplint nit)
2. #576 feat-color-space-tagging — 标记 API(pixmap/image/paint 双重载/
   capi bitmap_c、标记测试)
3. #577 feat-color-space-codec — PNG 元数据(低层 libpng、gAMA 倒数、
   cHRM D50、CRC、缩放标签、codec 测试)
4. #578 feat-color-space-decode — 渲染消费(sampler+预乘、NEON 回退、
   WGSL decode、golden、web decode 用例)

**拆分正确性证明**:拆分链 tip 对 b191974d 在 #555 全部 41 个文件上
`git diff` 为 0(逐字节一致);四层各自独立编译+全量测试+门禁通过
(unit: A 1083 / B 1089 / C 1102 / D 1107,逐层递增即各自增量)。
color_space_test.cc 按测试名截断:类型 1-379 行在 A,380 行起 3 个
标记测试随 B。

**base 说明**:四分支同源于 origin/main 最新(4bf1e9ae 之上 main 又
进了 wgx/fontconfig 等);#555 老 base 是 4bf1e9ae,新链 diff 天然
不含 main 新进内容。

**#570**:已 rebase --onto D(9ee742c5,零冲突,tree 等价 unit 1121),
描述 stack 指向更新。#571/#572 适配时同样挂 D 之后;#572 的 7 个 web
target 用例基线按 §10.1 记录重验。

**过渡态**:C 合入后、D 合入前,带非 sRGB 元数据的 PNG 显示与 main 有
字节级差异(main 本来就显示错这类文件;D 合入后正确)。

### 11.1 流程规范:拆分 PR 逐个开(用户指示,2026-11-12)

以后把大 PR 拆成多个时**不要一次性全部创建**,一个一个来:开第一个 →
review/合入 → rebase --onto → 再开下一个。当前已纠正:#576/#577/#578
已关闭(评论注明按序重开,分支保留),只留 #575 开放;#570 转 draft 等
链尾。#555/#575/#570 描述的 stack 措辞已改为"逐个开"。

后续节奏:#575 合入 → rebase tagging 链、重开 #576(或新号) → 依次
codec、decode → 最后 #570。

### 11.2 色度数学迁入核心层(2026-11-12)

用户指出 CodecGamutFromChromaticities / CodecChromaticitiesFromGamut 本质是
color space 数学,不该放 codec 模块。已迁为 ColorSpace 公开静态方法
(`GamutFromChromaticities` / `ChromaticitiesFromGamut`,对齐 MakeRGB 先例):
- A(#575)新增方法+实现(Bradford/XYZFromXY 常量入 color_space.cc 匿名 ns,
  复用 ColorSpaceInvert3x3/Multiply3x3)+ 两个测试从 png_codec_test.cc
  迁入 color_space_test.cc;
- C 分支删 codec_priv 的数学副本(~200 行:Invert3x3/Multiply3x3/kBradford/
  BradfordAdaptation/两函数),png_codec 改调核心 API。

**踩坑(重要)**:
1. 拆分 A 的首 commit(826c8709)漏提交了三个 CMake wiring
   (src/test/include 的 CMakeLists 行)——python 改的文件没 git add,
   当时构建用的是 core 分支的陈旧 CMake 缓存所以测试侥幸通过,#575 CI
   必红。教训:**python 修改的文件必须显式 add 并核对 commit stat**;
   构建验证前若怀疑缓存,删 .o 或重新 configure(test-runner 去掉
   --no-configure)。
2. rebase 到 amend 过的 base 时必须用 `--onto <旧base>` 只重放自己的
   commit,普通 rebase 会把旧 base commit 也重放产生重复。
3. #570 冲突解决时误取 ours 丢了 v2 测试(1121→掉数)——合并规则:
   v2 测试文件 + A 的色度测试追加;amend 前 unit 数与搬迁前核对。

各分支验证:A 1085 / B 1088 / C 1099 / D 1104 / core 1121+golden+web+门禁。
#575 已更新(23a12731);B/C/D 分支已更新(PR 仍关闭按序重开);core 10651b89。

### 11.3 #575 自查(数学/白点/注释/测试,2026-11-12)

对 #575 类型层做四维自查,结论与修复:

**数学核对(全部通过)**:
- sRGB TF 常数(2.4/1.055/0.055/12.92/0.04045)✓;线性 TF {1,1,0,...} 负值
  走镜像 -pow(-x,1)=-x 恰好全域恒等 ✓
- sRGB gamut 16-bit ICC 常数行和 = 63147/65534/54073 → (0.96354,
  0.99997, 0.82510) ≈ D50(ICC 舍入内)✓
- kDisplayP3Gamut 行和精确 (0.96422, 1.0000000, 0.8252100) = D50 ✓
- InvertTransferFn 闭式反转代数验证:(1/g, a^-g, -e·a^-g, 1/c, c·d+f,
  -b/a, -f/c),c==0 时退化为纯 gamma 逆(镜像分支兜底负值)✓
- Bradford {0.8951,...}、D50 XYZ、D65 xy 标准值 ✓
- GamutFromChromaticities 白点缩放/适配方向、ChromaticitiesFromGamut
  用行和实际白(自洽,sRGB ICC 舍入不引入误差)✓

**发现并修复 2 项**:
1. [数学] IsLinear 对 {1,1,0,c≠0/1,0,0,f≠0} 误报 true:旧条件 d==0 时
   无条件放行线性段,但 c≠1 时负值走 c·x+f 非恒等,Convert 会错误跳过
   decode。修复:线性段须 c∈{0,1} 且 f==0(c==0 镜像恒等仅 d==0 合法)。
2. [注释] DerefOrSRGB 说 "interned sRGB bytes",实现是本地静态默认
   构造值(字节相同但对象不同)。措辞改为 default-constructed value。

**测试补齐 2 个**(→ 21 个类型测试):
- NullTolerantHelpersTreatUndeclaredAsSRGB:ColorSpaceIsSRGB/Equals 的
  null 语义(null==null、null==SRGB() 数值、null≠P3、指针快路径、
  反序列化 sRGB 值==null)
- IsLinearRequiresIdentityEverywhere:幂段恒等+线性段非恒等
  (c=0.5 缩放/f=0.25 偏移)的负例

各分支重 stack 验证:A 1087/B 1090/C 1101/D 1106/core 1123+golden,
门禁过,已推送(#575=9a4700d2)。

### 11.4 #575 白点注释与测试补强(2026-11-12)

用户问 D50/D65 是否都在用 → 都在用但角色相反:D50=内部连接空间(入方向
适配目标,转换中相消),D65=外部发布口径(出方向还原目标)。补强:

- 注释 3 处:kD50XYZ/kD65 上方加"白点桥"总注释(两个方向+转换相消);
  kD65 说明非 D65 原生白的空间会被规约到 D65 参考(精确/Bradford 近似);
  ChromaticitiesFromGamut hpp 契约同步。
- 新测试 NonD65NativeWhiteIsStoredAtD50AndExportedAtD65(D50 白+sRGB
  原色),利用 Bradford 两个性质做精确断言:白→白精确映射(任意原生白
  的行和恒等于 kD50XYZ ±1e-4)+ 传递性(D65 规约后重导入精确还原矩阵
  ±1e-4)。原缺口:此前所有测试白点都是 D65。
- 已知浮点坑记录(未触发,选参时避开):输入方向 x+y>1 拒绝用严格大于,
  原色落在 x+y==1 边界时(如 WideGamutRGB 红 0.7347+0.2653)float 下
  和可能为 1.00000006 被拒;zero-z 容差只在提取方向(1e-4)。
- 流程修正(用户指示):#575 定稿前不动下游分支,避免每次 restack。
  本轮只 push type(e3108311,unit 1088/22 测试)。

### 11.5 color_filter wire 位级测试(2026-11-12)

用户注意到 #575 改了 color_filter.cc(FlattenToBuffer 常数来源统一为
ColorSpace 工厂)。检查发现:**该路径此前零测试覆盖** —— golden 两个
gamma 用例(SRGBToLinear/LinearToSRGB_Translucent)走 FilterColor 渲染,
unit 只有 Apply/Compose,序列化测试不涉及 gamma filter。而改动承诺
"wire 不变"(老版本写的 buffer 仍要能读),必须钉住。

补 SRGBGammaColorFilterFlattenTest.WireMatchesLegacyConstants:
- WriteBuffer 桩捕获 WriteByteArray 字节;
- 期望字节用**旧常数表字面量**构造(2.4f/1÷1.055/… + 16-bit 定点
  gamut),memcmp 位级断言,两方向 filter × src/dst 两 chunk;
- 位级等价的依据:1/65536 是 2 的幂,除法与乘以 1.52587890625e-5f
  的 IEEE 结果相同;
- 红验证:临时把 gamut 换全精度值 → 4 断言红;恢复 → 绿。
  即将来任何人改 ColorSpace 常数(精度/来源),wire 兼容立即报警。

ColorFilterGolden 2/2 过(渲染路径未被常数来源统一影响)。
#575 = 61e0c5ab(unit 1089/23 个类型+filter 测试),只动了 type 分支。

### 11.6 Bradford 重构(review 建议,2026-11-12)

#575 review 建议 BradfordAdaptation 改为显式三步式(可读性)。已采纳:
- kInverseBradford 文献常数(先独立验证 B·B⁻¹ 偏离 I ≤8.45e-8,与运行时
  求逆差异 ~1e-7,远小于最紧测试容差 ±1e-5);
- Multiply3x3Vector helper + 显式对角 scale 阵 + B⁻¹·S·B 两步乘
  (用 ColorSpaceMultiply3x3);
- 保留原实现的退化白点防御(cone 非正/非有限 → false;reviewer 版本
  会静默 NaN 传播)——review 建议采纳时防御检查不能丢。

行为等价证据:unit 1089 全绿,±1e-5 往返断言未受 1e-7 常数差异影响。
#575 = 3f3ecfb1。

### 11.7 CI 浮点平台差异修复(2026-11-12)

#575 CI(Linux glibc + Windows MSVC)挂 ChromaticitiesFromGamutUndoesThe
Adaptation 往返断言;macOS 全绿。根因即 §11.4 记录的坑:**P3 红原色
x+y 恰好=1.0,提取方向有 1e-4 容差、输入方向是严格 >1.f 拒绝**——
Linux/Windows 的除法舍入让提取值 x+y 微超 1(几 ulp)→ 往返被拒。
(LNK4197 是 warning,windows job 实际也挂在同一 UT 断言。)

修复:GamutFromChromaticities 输入检查加对称 1e-4 容差;z 分量随之
可微负(≈-1e-4),矩阵求解不受影响。补平台无关回归断言
(few_ulps_over 0.680006+0.32 接受 / past_epsilon 1.0002 拒绝),
红绿验证过(撤销容差 → 断言红)。#575 = 0b316099。

教训:边界值(x+y==1)的双向容差必须对称,跨平台浮点差异靠 CI 兜底,
本地单平台绿不代表边界安全。

### 11.8 链推进:#575 合入 → 第二个 PR(2026-11-12)

#575 squash 合入为 e4b5a7c0。tagging 分支 rebase --onto 新 main
(旧 base 300681b1,零冲突),unit 1103/门禁过,push b2c56d77。
#576 reopen 被 GitHub 拒(API 限制),新开 **#579**(描述按 #575 先例
简化,无历史叙事)。旧 #576 留关闭态,其评论指向链即可。

后续节奏(逐个):#579 合入 → codec 分支 rebase onto main → 开 PR →
decode 分支同理 → core(#570)/#571/#572。

### 11.9 #579 review 采纳:MakeHWImage 重载 + GetColorSpace 非虚(2026-11-12)

用户两点建议均已实施(tagging 分支 c69f568e,#579 更新):
1. MakeHWImage 拆两个显式重载(单参=按 sRGB / 两参=带源空间),与
   Paint setter 的"显式双版本无默认参数"约定统一;单参委托两参传
   nullptr。
2. GetColorSpace 去 virtual:基类存 protected 构造初始化的
   color_space_ 成员(PixmapImage 构造时继承 pixmap 的 tag,
   TextureImage 显式传入,Deferred/Promise 默认 null),普通内联
   访问器。子类删 override/成员;子类内部经 GetColorSpace() 访问。
   **#570(core)rebase 时注意**:v2 的 Image::GetColorSpace 虚形式要
   同步去掉(hw_layer 等调用点语义不变,非虚调用)。
unit 1103/golden/门禁过。

### 11.10 #579 测试补齐与丢失修复(2026-11-12)

用户问"改动涉及 pixmap 是否需要加测试"→ 排查发现两个问题:

1. **B 分支早前丢失了 3 个标记测试**(PixmapColorSpaceMetadata/
   PixmapImageCarriesColorSpace/PaintSetColor4fConvertsToWorkingSpace)。
   丢失点:IsLinear 轮 B rebase 的两轮手工重建;当时 unit +3 被 capi
   bitmap_c_test 的 3 个测试顶上数字,掩盖了丢失。
   **教训:rebase 手工重建文件后,不能只对总数 —— 要按测试名核对
   该分支应有的增量。**
2. 补快照语义测试 ImageTagIsFixedAtConstruction(GetColorSpace 非虚化
   的行为契约:image 在构造时定格 pixmap 的 tag,事后 retag 不影响;
   与 texture 路径一次性上传对齐)。红验证:临时构造丢 tag → 红。

#579 = e8317ed0(unit 1107 = 1103 + 3 恢复 + 1 快照,门禁过)。

### 11.11 #579 paint 测试补齐(2026-11-12)

用户问 paint 改动是否要补测试 → 现有 PaintSetColor4fConvertsToWorkingSpace
已覆盖转换主路径(4f/打包/跨 gamut/plain 原样),缺口三处,补
PaintSettersHaveNoStickySpace:
1. SetColor(Color) 单参重载的 byte-identical(GetColor4f 回读打包相等);
2. 半透明色(alpha≠1)经 set-time 转换 alpha 原样(此前只测过 alpha=1);
3. **无粘性 space**(当初 API 改造的动机):spaced setter 之后调
   deprecated float setter,值不经任何残留转换落地 —— 钉死
   "space 在设置时消费"契约,防止将来退回 sticky 语义。
#579 = a9d5c740(unit 1108,门禁过)。

### 11.12 codec PR 链条推进(2026-11-12)

#579 squash 合入 = f17c9a7d。codec 分支 rebase:
`git rebase --onto f17c9a7d 28c58ef3 feat-color-space-codec`
→ 29387754,零冲突(4 文件与 #575/#579 review amend 无交集,
重放前 grep 确认无 Codec*From* / MakeHWImage 残留引用)。

验证:PNGCodecTest 16/16 → 全量 unit 1119(= main 1108 + 11 新增 codec
测试),cpplint + clang-format 过。force-push dev/feat-color-space-codec,
PR #580 已创建(正文简化风格,无 #555/Skia/skcms 字样)。

codec 行为要点(PR 正文口径):
- 读侧 sRGB chunk 与 iCCP 都回落 sRGB 后者因 ICC 无法归约为参数形式;
  gAMA 记倒数(编码 gamma → 解码指数);cHRM 走核心
  GamutFromChromaticities(D50 适配在核心),退化 primaries 保 sRGB gamut。
- 写侧只写 chunk 能精确表达的:纯幂 gamma → gAMA;非 sRGB gamut →
  cHRM(ChromaticitiesFromGamut 反适配回原生 primaries)。
- 像素保持 source-encoded,缩放解码传播 tag(ResamplePixmap)。

下一步:#580 合入后 rebase decode 分支(old-base 是 decode 分支上
codec commit 的等价 SHA,同样 --onto 只重放 decode commit);
再 #570 core draft;最后 #571/#572。

### 11.13 #580 自查:逻辑/测试审查 + 其他格式评估(2026-11-12)

用户要求检查 codec 逻辑与测试,并问其他格式是否需要适配。

**发现并修复一个真实问题:16-bit PNG 字节回归**
- main 的简化 API(png_image)对 16→8 用 png_set_scale_16
  (pngread.c:3813,round(V/257));新代码误用 png_set_strip_16(丢低字节)。
- unit/golden 均无 16-bit 用例 → 静默行为变化,违反"默认路径与 main
  字节一致"原则。修复:换 scale_16 + 注释。
- 回归测试 SixteenBitPNGScalesInsteadOfStrips:手工构造 1x1 16-bit RGB
  PNG,zlib 流用 stored deflate 块(免压缩器、字节确定),样本选
  strip/scale 有区分度:0x00FF→1(strip=0)、0xFF00→254(strip=255)、
  0x8000→128(两者同)。红绿验证过(strip 下两通道红)。
- unit 1120/1120,门禁过,amend = 113e05dd,PR 正文已更新。

**审查通过的关键点(不再改)**:
- setjmp 路径手工清理正确:buffer 所有权在 png_read_end 之后才转移进
  Data,png_read_end 失败 → longjmp → free(buffer) 无 double-free;
- gAMA 倒数、0<gamma<10 消毒、cHRM 退化回落 sRGB gamut;
- 写侧 pure_power 判定要求 c==0 虽略保守(d==0 时 c 数学上无关),
  但符合"只写 chunk 能精确表达的"文档化限制;
- CRC 损坏 ancillary chunk 依赖 libpng 默认 warn+discard,测试实证;
- C ABI set 路径 MakeRGB 校验兜底(PARAMETRIC 非法 → 拒绝 → 保持未标记)。

**已知可接受限制**:带非 sRGB iCCP 的 PNG,main 会被 libpng 按 profile
gamma 转换,新路径给源编码字节 + sRGB tag(文档化回落);根治需 ICC 支持。

**其他格式结论:不需要也无法在本链条适配**
- JPEG:颜色元数据只有 ICC profile(APP2),无参数化 chunk;
- WebP:只有 ICC chunk(demux API 才可取);
- GIF:格式本身无颜色元数据;BMP:无实际使用的元数据;
- PNG 是唯一带参数化 chunk(gAMA/cHRM/sRGB)能精确映射 ColorSpace
  参数模型的格式。其它格式适配 = ICC 解析/近似子系统,超范围;
  其现状(未标记=sRGB)与 main 一致且有定义。ResamplePixmap 的 tag
  传播是格式无关的,若上游标记过 pixmap 已自动覆盖它们。

### 11.14 #580 第二轮 review:BUS-1~4 修复(2026-11-13)

Handoff 包 /Users/bytedance/Downloads/skity-color-space-codec-113e05dd-handoff,
四项全部成立(逐项本地复现验证;BUS-1 泄漏 Debug 不现、Release 现征 — UB 特征,
reviewer 实测 12,632,064 = 12×(512²×4+512×8) 精确吻合)。

**BUS-1 泄漏(P1)**:buffer/rows 是 setjmp 后赋值的非 volatile 局部 → longjmp
后值不确定(C11 7.13.2.1)→ free 错误地址。修复:指针本身 volatile
(uint8_t* volatile)。教训:查 longjmp 清理只查 double-free 不够,
还要查"值是否跨 jmp 有保证"。自查漏掉,reviewer 抓到。

**BUS-2 单独合入回归(P1,集成条件)**:main 简化 API 对 gAMA 文件做的是
**正确**的 sRGB 输出转换 —— commit message 里 "main already displayed
incorrectly" 对 gamma 文件不成立(只对 cHRM 宽色域成立),已改正。
处理:配对合入。decode 分支 rebase 到修复后 head,DrawImage 测试
(ExistingLinearPngStillDrawsCorrectly)收编进 decode 分支:codec 单独=64(红,
已复现),stacked head=137(绿,已验证)。#580 与 decode PR 背靠背合入。

**BUS-3 丢 ICC fallback(P2)**:png_get_iCCP 成功即 return false 丢掉
gAMA/cHRM。PNG 规范让编码器在 iCCP 旁写近似 gAMA/cHRM 给非 ICC reader。
修复:删 early return,未读的 iCCP 让 fallback chunk 生效。

**BUS-4 unified 链接(P1)**:core_bridge.hpp:11-14 契约(codec 只 import
核心 C ABI,点名 Pixmap::SetColorInfo 也要走桥)。修复:
- capi 新增 skity_color_space_gamut_from_chromaticities /
  chromaticities_from_gamut(数学留核心,过边界调用);
- png_codec 两处 + ResamplePixmap(SetPixmapColorSpace 桥,GetColorSpace
  是 header-inline 可留)换桥;
- 新 C 符号走 _skity_* 通配自动导出,无需改 cmake;
- 新增 CapiColorSpaceMath 回归测试。

验证:Release 构建 20/20(含泄漏回归);unified 构建(SKITY_UNIFIED_LIB=ON
+ CODEC_MODULE=ON,reviewer 同款 cmake 配置)链接成功 + capi 4/4;
Debug 全量 1123/1123;decode 分支(rebase 后 f6f51d46)unit 1129/1129 +
golden ColorSpace 3/3 + 门禁过。codec=30d043bd,decode=f6f51d46 已推。

**注意**:build-business-unit / build-business-unified 两个构建目录是
本轮按 reviewer 配置新建的(在仓库根,非 out/)。

### 11.15 #580 修复后自查补漏(2026-11-13)

用户要求结合 handoff 文档再自查。逐项验收标准核对结果:

1. **cabi_only.map 是 skity_* 通配** —— 新 C 符号 Linux 统一库也自动
   导出,无需登记(此前只在 macOS 验证过,这里补上了 Linux 侧的确认)。
2. **decode 分支 golden 只跑了 ColorSpace 过滤** → 补跑全量:
   golden-shape 269/269 + golden-text 11/11 —— BUS-2 验收
   "正常 sRGB 图片保持不变"的全量证据(既有内容渲染零变化)。
3. 补两个定向测试:
   - codec 分支 IccOnlyFileStaysSRGB(仅 iCCP 无 fallback → 保持 sRGB,
     钉住 BUS-3 修复的另一侧;该侧行为修复前后都对,无红态,属边界
     文档化测试);
   - decode 分支 UntaggedPngDrawsUnchanged(无 chunk 普通图绘制原样,
     与 ExistingLinear 137 配对,把 BUS-2 契约钉全:转换侧+no-op 侧)。
4. 手工冲突处理翻车一次:git conflict 标记用 python replace 替换,
   把 IccOnly 的收尾 } 和 DECODE-SIDE 残留搞坏 → 编译错误才发现。
   教训:**冲突标记永远直接 Edit 工具逐处修,不要 sed/python 批量替换**;
   修完立即编译验证再 continue rebase。

最终状态:codec=eb378d43(unit 1123),decode=a1fecc3d(unit 1131,
golden 269+11,门禁过),均已推送,#580 正文 SHA 已同步。

**遗留(非本轮)**:BUS-1 泄漏测试非 macOS 平台 SKIP,CI Linux 需对应
平台泄漏检测补充(README 明示 SKIP 不算已验证)。

### 11.16 decode PR 开启(2026-11-13)

#580 squash 合入 = 7c75576a(main 中间还进了 #573 path perf)。
decode rebase --onto origin/main eb378d43 → e521df43,零冲突。

验证:unit 1137/1137,golden-shape 269/269,golden-text 11/11,
门禁过。PR #581 已创建(15 文件,+671/-18)。

PR 正文要点:CPU 采样(unpremul 域 + NEON 回落)、GPU runtime uniform
解码(零 shader 变体)、与 #580 背靠背的配对契约(线性 137 / 无 chunk
原样,两个测试名都写进正文)。BUS-2 的回归窗口随 #581 合入关闭。

链条状态:#575 ✅ #579 ✅ #580 ✅ → #581 review 中 → 之后 #570
(feat-color-space-core draft,需 rebase 到链条末端,v2 虚函数 GetColorSpace
形式已过时要删)→ #571/#572 适配(#572 需 API 重做:sticky
SetColorSpace → 工厂重载)。

### 11.17 #581 自查补测试(2026-11-13)

用户要求自查补测试。逐行为对照生产 diff:

**已有覆盖**:CPU premul→unpremul 域+回乘(线性半透明)、NEON span 回落
(9px nearest)、P3 解码往返、sRGB 位不变;GPU shader 文本比对(含负数
mirror、re-premul 理由注释)、不透明 P3/线性回读、基线图、ScalePixels
保 tag;配对契约(线性 137 / 无 chunk 原样)。

**补 4 个测试**:
1. ZeroAlphaPremulSampleKeepsExactZeros(CPU)——钉 sampled.w>0 守卫的
   精确零契约。红绿结论:本平台 NaN→uint8 恰得 0,**红不可复现**
   (守卫防的是 cast UB 的平台相关性,测试在其他平台有意义);
2. CubicSamplingStillConverts(CPU)——钉重构后 cubic 分支也转换
   (恢复旧 early return 即红,已验证);
3. PremulTextureDecodeRepremultiplies(GPU 回读)——半透明 P3 premul
   纹理,期望值用 CPU 同款数学独立计算;含全透明半区保精确零
   (钉无条件 re-premultiply 的黑点不泄漏论证);
4. (同上测试内)透明像素零断言。

GPU 红绿:shader 里 cs_rgb * color.w 临时改 * 1.0 → 红 ✓ 恢复绿 ✓。

最终:decode=92d1b568,unit 1139/1139,golden-shape 270/270,
golden-text 11/11,门禁过,已推送,PR 正文数字已同步。

### 11.18 #581 补 GPU 端到端 golden(2026-11-13)

用户追问"codec 解码出来的纹理不是有 color space 么"——对:GPU 全链路
(PNG chunk → codec tag → 纹理 → WGSL 解码 → 回读)此前 4 个 GPU golden
都是程序构造 tagged pixmap,没有从真实 PNG 出发的。补
CodecTaggedPNGDecodesOnGPU:g2.2+P3 encode→decode→GPU draw→回读,
期望值从解码后的字节+tag 独立计算(与 chunk 量化解耦),再断言整链
保源色(±4/255)。

**红绿发现的链路事实**:GPU 绘制路径的 tag 流是
MakeImage→image.cc:210(pixmap->GetColorSpace → TextureImage ctor)→
wgsl_texture_fragment.cc:71((*image)->GetColorSpace()),
**不走** PixmapImage ctor(165 行,CPU-only)。第一次红绿改错对象
(165)测试仍绿——这本身验证了链路分离。改破 210 → 红 ✓ 恢复绿。

**测试编写翻车一次**:用 uint32 直写 RGBA 字节内存,Color 是 ARGB
打包,字节序把 R/B 换了 → 通道错乱挂掉。改逐字节写解决。
教训:写 pixmap 像素要么 SetPixel 要么逐字节,不要 uint32 强转。

最终:decode=adb3e000,unit 1139/1139,golden-shape 271/271,门禁过,
已推送,PR 正文更新。

### 11.19 #581 与 main 冲突处理(2026-11-13)

原因:main 新进 #582(text 重构,glyph_run 拆分 + 新增 5 个 glyph run
文件 + fake_gpu_context 测试基建),与 decode 分支唯一真实文件交集是
test/ut/CMakeLists.txt —— 双方在同一列表追加测试条目(经典 both-append
冲突)。wgsl_texture_fragment/bitmap_sampler/hw_canvas 等生产文件无交集。

解决:rebase --onto 9242b570,冲突单处,手工保留两行(注意列表字母序:
gpu/ < graphic/,#582 的位置才是对的;bitmap_sampler_test.cc 在 graphic
块内排 bitmap_test 前)。

验证:unit 1144/1144(含 #582 新增测试),golden-shape 271/271,
golden-text 11/11(text 重构与 decode 无回归交互 —— glyph atlas 纹理
无 tag,decode 块跳过),门禁过。decode=e81997a4 已推送。

### 11.20 #581 第二轮 review:CS-01~03 修复(2026-11-13)

Handoff /Users/bytedance/Downloads/skity-color-space-decode-e81997a4-handoff,
三项全部成立并本地复现(Release CPU 3 挂 1 过 + Metal 挂,与 reviewer 一致)。

**CS-01 premul 域溢出(P1)**:P3→sRGB 超出 [0,1],premul 分支直接乘回
alpha → R>A 破坏规范预乘,NEON 饱和加法与标量 packed 加法行为分叉
(255 vs 10)。修复:CPU sampler 在 unpremul 域 clamp 再乘回;GPU shader
同位置 clamp(encode 后、re-premul 前)保持两端一致;不碰 Convert 的
extended-range 契约。新增 PremulOutOfGamutClampsLikeCPU(黑底读回,
期望用公开 API 数学算出——golden case target 不继承仓库根 include,
不能 include src/ 私有头,教训)。红绿:去 shader clamp → 红 ✓。

**CS-02 ScalePixels 目标空间(P2)**:契约 = 未标记/sRGB 目标 → 解码到
工作空间(现行为);同空间目标 → 直通采样不转换(GPU 走 plain
shader:MakeHWImage 传 nullptr tag);其余声明组合 → 明确 false 拒绝
(ScaleDestinationSupported,CPU/GPU 两路径共用)。reviewer 的
ScalePixelsHonorsLinearDestination 按"支持声明目标"契约直接通过;
另补 UnsupportedScaleDestinationIsRejected 钉拒绝契约(dst 未被写入)。

**CS-03 每像素重复 prepare(P2)**:PreparedConversion/PrepareConversion/
ApplyConversion 从 color_space.cc 匿名命名空间提升到
color_space_priv.hpp;sampler 构造时准备一次(GetColor 只 apply)。
实测(Release,1024² P3 unpremul,1M GetColor,5 轮取中位):
修复前(临时换回逐像素 Convert)33.22ms → 修复后 20.67ms(−38%,
1.61×),与 reviewer 隔离测量 1.57× 吻合。临时基准测完即删,
数值走 RecordProperty 从 gtest_report 读。

**环境坑**:build-business-unit 沿用 codec 轮配置没有
WGX_SPIRV_BACKEND=ON → ShaderWriter 7 个 WriteToSpirv 挂
(REPRODUCE.md 有预警,与本修复无关);补开后 44/44。

最终:decode=9c03d1fe。Reviewer 回归范围 44/44(Release+Debug 双构建),
全量 unit 1149/1149,golden-shape 273/273,golden-text 11/11,门禁过,
已推送,PR 正文同步。

### 11.21 #581 cpplint job 挂因(2026-11-16)

CI 的 cpplint_check job 实际是 cpplint + clang-format 复合:cpplint 本身
PASSED,挂的是 format,文件 = reviewer 补丁新增的
color_space_decode_review_test.cc。

**根因**:我本地跑 format 门禁时该文件还是未跟踪状态(??),
tools/code_format_check.py 只看 git diff 里的已跟踪文件 → 本地漏检,
提交后 CI 才看到。与 11.5 CMake 教训同类:**新增文件必须先 git add
再跑门禁**(新增文件三步:add → 门禁 → 提交)。

修复:clang-format 该文件 → amend(d16d01b1)→ 门禁过 → 已推送。
另:/tmp/cpplint-venv 隔天失效,下次需重建 venv。

### 11.22 #570 core rebase 到链尾(2026-11-16)

#581 合入 = c34f0cb1(main 还进了 #569 PathBuilder + #582 text 重构)。
feat-color-space-core(旧 52604879,基于旧链)rebase --onto origin/main,
10 文件冲突,语义解决要点:

1. **模型更替**:paint 从 #579 的 set-time 转换 → per-target 记录
   (GetFillColorSpace/GetStrokeColorSpace + draw 时按 target 解析)。
   PaintSetColor4fConvertsToWorkingSpace/PaintSettersHaveNoStickySpace
   两个过时测试删除——PaintColorSpaceIsExplicitPerColorSet 已完整
   覆盖 no-sticky 契约(核对过内容)。
2. **PreparedConversion(CS-03)被 ColorXformSteps 取代**:color_space.cc
   的定义与 priv 头声明删除(泛化为 CPU/WGSL 共用的折叠描述);
   sampler 用 steps_ 但 **CS-01 clamp 保留**(unpremul 域 clamp 移植到
   steps_.Apply 路径);fragment 同理(分阶段 cs_flags 版 + clamp)。
3. **v2 虚函数 GetColorSpace 删除**:DeferredTextureImage 的 override +
   遮蔽成员删除;base 加 protected SetColorSpace(构造后重打 tag 用),
   派生公有 SetColorSpace 委托。
4. **#582 text 重写的移植**:glyph_run 取 HEAD 新结构;SetColor4f 带
   记录空间的模式移植到 atlas_glyph_run.cc(#582 新文件,原来漏);
   transformed_mask 取 HEAD + SetFillColor(Color4fToColor(...), space)。
5. **golden 文件拼接翻车一次**(HEAD 块被粘进未闭合测试体内)→
   放弃修补,干净重建:main 版本原样 + 旧分支 8 个 per-target 测试追加
   (+Gamma22TargetSpace helper + shader/color_xform include,
   case CMakeLists 已有 INCLUDES ${SKITY_ROOT})。
6. 旧分支 color_space_test 重复测试(Pixmap* 两个,#579 已在 main)
   删除(CMake add_test 重名报错暴露)。

验证:unit 1185/1185(净增 36),golden-shape 283/283(旧 273 基线
零变化 = 默认 sRGB target 渲染不变 + 10 新),golden-text 11/11,
门禁过。core=089f5154 已推送,PR #570 正文重写(无历史叙事)。

### 11.23 #570 补 target 基线图 golden(2026-11-16)

用户指出缺 target 的 golden——核实:8 个 per-target 用例都在(数值断言,
P3/linear/gamma22 target,SetTargetColorSpace + 复位 nullptr 模式正确),
真正缺的是 **PNG 基线图形式**。补 DisplayP3TargetDecodeBaseline:
同 DrawDecodingImages 画到 P3 target,p3_target_decode.png 基线钉住
(P3 图直通 + 线性图 P3 编码的可视化工件)。红绿:target 换 linear → 红。
golden-shape 284/284,core=1f07848d 已推送,PR 正文同步。
web harness 的 target 用例属 #572 范围(旧分支已有 web P3 case+基线)。

### 11.24 #570 全分支自查(2026-11-16)

用户要求再自查。系统过一遍自动合并未细看的文件,结论:

**发现并修复 1 个真实 bug(sw_canvas.cc:809)**:
GenerateBrush 的 stroke 标志来自绘制 pass,而 GetColorSpace() 按
style_ 选槽位 —— kStrokeAndFill 的描边 pass 会拿 stroke 颜色配
fill 空间解析(混合空间 paint 下颜色错)。修复:改用带 target 的
槽位 getter GetStrokeColor/GetFillColor(bitmap_space)(GPU 侧
wgx_utils 本来就是这个写法)。回归测试
StrokeAndFillResolvesEachSlotWithItsOwnSpace(线性 stroke 0.5→188,
错配时 128);红绿验证过。测试几何第一次还写错了(6px stroke 覆盖
了整个 fill 区,采样点全错)——2px stroke + (5,8)/(8,8) 采样才对。

**核对通过的关键点**:
- setter 模式全库只剩 DrawColor(正确:无空间参数=工作空间);
  #582 新 glyph 文件无遗漏;
- ScalePixels 契约保持(CS-02),render target 描述符内嵌选项默认
  null,不受 golden env 的 SetTargetColorSpace 污染;
- target 流向:surface 创建时 render_options.color_space →
  gpu_surface_impl → hw_canvas:827 draw_context.target_space →
  layer/precompile/fragment 逐层传递,非 context 全局;
- pipeline key:decode 变体单一位 + 运行时 cs_flags uniform ——
  所有 stage 组合同一 shader 文本,无 key 冲突;
- 预编译:PrecompileContextImpl 不设 target(null=sRGB),程序
  color-independent(纯色走 uniform 不烧文本)→ 跨 target 复用成立;
  decode 变体不在预编译范围(与 #581 相同,非回归);
- CPU 侧:渐变 stop 解析到 target(注释明确 stop 级空间是未来扩展);
- paint deprecated setter 记录 sRGB + 清空间(no-sticky 保持)。

最终:core 单 commit,unit 1186/1186,golden 284/284 + 11/11,
门禁过,已推送。

### 11.25 GPU 链路转换盘点 + 彩色图集缺口(2026-11-16)

用户问"GPU 渲染链路都做了颜色空间转换吗"→ 全链路盘点:

**覆盖**:图像/纹理(WGSLTextureFragment 分阶段 decode→target,运行时
cs_flags)、纯色(wgx_utils 构造时 Get{Fill,Stroke}Color(target))、
顶点色(rrect:405 / text:257 写顶点时解析)、渐变 stops(构造时,
stop 级空间为 #572 扩展)、文本 A8/SDF(coverage 无空间)、
SaveLayer(层纹理带 target tag + filter 子上下文继承)、
滤镜/模糊(空间运算,输入已进 target 编码)、设备混合(目标编码空间)。
全库裸 GetColor4f( 扫描无残留。

**缺口(用户决定下个 PR 修)**:彩色字形图集 kColor/kColorSwizzleRB ——
wgsl_text_geometry.cc:162 textureSample(uTextAtlas) 原样输出,
sRGB 编码的 emoji/彩色字体位图画到非 sRGB 目标不转换(颜色错)。
CPU SW 文本图集 blit 大概率同样缺口(修复时需验证)。

**修复 PR 工作项**(在 #570 合入后开):
1. GPU:text geometry FS 对 kColor/kColorSwizzleRB 效果加 cs_*
   分阶段 decode(sRGB 图集→target),uniform 打包 + bind group;
2. CPU:SW 文本图集 blit 路径同款转换(先验证缺口存在);
3. 测试:非 sRGB 目标上彩色字形的 golden(P3 target emoji 用例)
   + 红绿;A8/SDF 回归确认不受影响;
4. 注意 #582 刚重写文本结构,text geometry 是其新代码。

#570 PR 正文已加 "Known limitation (follow-up)" 一节。

### 11.26 #570 review:CORE-01~07 GPU 范围修复(2026-11-16)

Handoff /Users/bytedance/Downloads/skity-color-space-core-95f3c9a2-handoff,
7 项全部成立(CPU 3 挂 + GPU 4 挂本地复现)。

**范围决策(用户)**:CPU(SW 渲染器)后续整体重构 → 本轮只修 GPU 侧:
修 CORE-02/04/05(GPU)/06/07;延后 CORE-01(SW NEON)/CORE-03(SW
saveLayer)/CORE-05 CPU 半边。CORE-06 在 IO 层非渲染器、CORE-07 是
现网 UB(1 行),保留。

修复明细:
- CORE-02: hw_canvas 描边轮廓 SetColor4f(stroke color, stroke space)
  (不量化 Color32,reviewer 建议);
- CORE-04: MakeSnapshot → MakeHWImage(texture, impl surface space),
  static_cast<GPUSurfaceImpl&> 零公开 API 改动(friend 已有,
  surface_ 基类指针无 GetColorSpace);
- CORE-05 GPU: wgx_filter SetupBindGroup 用
  ColorXformSteps::Resolve(nullptr, target).Apply(constant) 再预乘,
  bind 时一次非逐像素;
- CORE-06: paint_flat 写侧 ColorSpace::Convert(recorded→nullptr)
  (wire 约定 sRGB;槽位核对过:C ABI getter 与 GetColorSpace 同为
  style-aware);
- CORE-07: sampler 恢复按值 SamplingOptions + 悬垂注释。

验证:GPU review 测试 4/4(修复前 4/4 挂,红绿成立);ASAN
(RelWithDebInfo, reviewer 同款配置)SRGBBitmapIsUnchanged 过无
stack-use-after-scope;reviewer 回归范围 80 单测 + 20 golden 全过;
全量 unit 1186/1186,golden-shape 288/288(+4 review golden),
golden-text 11/11,门禁过。core=40a77796 已推送。

延后项已写入 PR Known limitations(SW 渲染器重构时处理)。

### 11.27 SetFillColor/SetStrokeColor 去除 ColorSpace 重载(2026-11-16)

用户决定:这两个 API 后续会删除,不再支持 ColorSpace —— 单参版本
行为 = SetColor 不带空间(记录工作空间值,槽位空间置 null)。

改动:
- paint.hpp/cc 删除 SetFillColor(Color, sp)/SetStrokeColor(Color, sp)
  两参重载;单参实现为直接记录+null;SetColor(Color, sp) 内联双槽;
- 唯一外部调用点 transformed_mask_glyph_run.cc 改 SetColor4f
  (stroke→fill 拷贝,双槽同设等价);
- 混合槽位空间仍可用序列构造:SetColor4f(c, P3) 后 SetFillColor(plain)
  → fill=sRGB, stroke=P3。两个测试相应重写:
  PaintColorSpaceIsExplicitPerColorSet(槽位隔离契约改用序列构造)、
  StrokeAndFillResolvesEachSlotWithItsOwnSpace(混合空间构造改序列,
  红绿复验:SW 槽位 bug 回归测试仍能红 ✓);
- GetFillColorSpace/GetStrokeColorSpace getter 保留(读侧仍需)。

验证:ColorSpace.* 29/29,全量 unit 1186/1186,golden-shape 288/288,
门禁过。core=9bd1419d 已推送,PR 正文 API 描述同步(空间感知入口=
SetColor/SetColor4f)。

### 11.28 Paint 收敛为单一颜色空间(2026-11-16,未 push)

用户决策:SetFillColor/SetStrokeColor 直接重置**整个 paint** 的
colorspace(另一槽字节保留但按工作空间重解释)。推论:所有 setter
保持两槽空间恒等 → 成员合一。

实现(本地 c61b5006,等用户另两处改动后一起 push):
- paint.cc/hpp:fill_color_space_/stroke_color_space_ → 单个
  color_space_;单槽 setter(含 deprecated float)置 color_space_=null;
  SetColor/SetColor4f 设双值+单空间;operator== 同步;
- GetFillColorSpace/GetStrokeColorSpace 删除;GetColorSpace() 去掉
  style-aware 选择(恒等下无意义),文档改为"一个 paint 一个空间,
  单槽 setter 即整体重声明";
- 三处调用点(hw_canvas CORE-02 / atlas / transformed_mask)改
  GetColorSpace();
- 测试:PaintColorSpaceIsExplicitPerColorSet 重写为整体重声明契约;
  StrokeAndFillResolvesEachSlotWithItsOwnSpace 删除(混合状态不可
  构造,SW 槽位 bug 公开不可达——SW 按槽位解析代码保留,统一空间
  下仍正确)。

验证:ColorSpace.* 28/28,全量 unit 1185/1185(-1 删除测试),
golden-shape 288/288,golden-text 11/11,门禁过。

### 11.29 单槽 setter 改为转换而非重解释(2026-11-16,未 push)

用户追问:SetFillColor 时是否应转换 StrokeColor —— 对,转换更正确:
只碰 fill 不应改变 stroke 的显示值。重解释会让显示值漂移
(linear 0.5 描边 188→128);转换只规范化存储,外观保持。

实现(paint.hpp/cc,本地 5593ab2c):
- 私有 ResetColorSpaceToWorking(Vector& other_color):color_space_
  非空时把未触碰槽的值 Resolve(space→nullptr).Apply,再置空;
- 四个单槽 setter 体(float/Color × stroke/fill)统一走 helper;
- 文档更新(setter 注释 + 成员注释);测试补外观保持断言:
  stroke_before=GetStrokeColor(nullptr) 与 after 相等(±1e-4),
  裸值读出 0.73536(转换后)而非 0.5。红绿:去转换 → 红 ✓。

验证:unit 1185/1185,golden-shape 288/288,门禁过。仍未 push,
等用户的剩余改动。

### 11.30 删除 shader z==1 sRGB 常量 encode 分支(2026-11-16,未 push)

用户第二个问题确认:z==1(常量 cs_srgb_encode_channel)与 z==2
(参数化 cs_enc_tf)是同一件事的两种形式,sRGB 曲线可被 7 参数模型
精确表达 → z==1 是纯优化非必需,且 CPU Apply 本就总是参数化。
决定删除并重编号(cs_flags.z: 0=折叠,1=参数化)。

改动(本地 bf06394c):
- fragment shader:删 z==1 分支,z==2→z==1;cs_srgb_encode_channel
  发射删除(成死代码);
- 打包:encode_mode 变 bool;
- ColorXformSteps 删 encode_is_srgb_curve 字段 + Resolve 里 memcmp
  计算;
- writer 测试期望字符串同步;xform 测试 StageFolding 的 flag 断言删
  (折叠断言保留)。

验证:相关 68 单测,全量 unit 1185/1185,golden-shape 288/288
(基线零变化 = 参数化与常量曲线数值等价),golden-text 11/11,
门禁过。仍未 push(累计三处:11.28 单空间、11.29 转换、11.30 本项)。

### 11.31 ColorXformSteps → ColorTransformSteps(2026-11-16,未 push)

用户第三处改动:名字改中立。完整重命名(类型+文件+测试名):
- color_xform.{hpp,cc} → color_transform.{hpp,cc}(git mv 保历史);
- ColorXformSteps → ColorTransformSteps(include guard 同步);
- color_xform_test.cc → color_transform_test.cc,测试套名
  ColorXformSteps.* → ColorTransformSteps.*(TransformPixels 不变);
- 14 个文件引用更新(src CMakeLists、sampler、paint、fragment、
  wgx_filter/utils、sw_canvas、golden、ut CMakeLists)。

过程坑:重命名后行宽漂移导致 format/cpplint 挂,且已 amend 进
commit——format 后重新 amend 修正(9ae20b40)。教训:批量重命名后
先 format 再 amend。

验证:重命名测试 12/12,全量 unit 1185/1185,golden 288/288,
门禁过。本地累计四处未 push:11.28/11.29/11.30/11.31。

### 11.32 四处调整 push(2026-11-16)

用户确认:全分支 format 无漂移 → push 9ae20b40(unit 1185/1185,
cpplint 过)。PR 正文 ColorXformSteps → ColorTransformSteps 同步。
paint API 描述在 11.27 已更新过(setter 行为 11.28/11.29 又变,
正文的 "single-slot setters record in the working space only"
仍准确——转换是实现细节,对外契约不变)。

### 11.33 #570 合入 + 彩色字形缺口调查搁置(2026-11-17)

#570 squash 合入 = d8902cdf。main 另叠 #584(text atlas bearings)/
#585(path bounds)。

**调查结论(未改代码,分支 feat-color-space-text 已删)**:
- GPU 缺口确认仍在:wgsl_text_geometry.cc WriteFSColor 对
  kColor/kColorSwizzleRB 直接 textureSample 原样输出(顶点色已按
  target 解析于 :257,图集位图本身不转换)。修法已想好:text
  struct uniform(group(3) binding 6,镜像 ImageColorInfo 的
  cs_flags/tf/gamut/enc 成员)+ GetMainKey variant bit 2 +
  OnGenerateDrawStep 处 Resolve(nullptr sRGB 图集→target_space)传入
  geometry 构造;precompile 不传 target(保持 sRGB 程序,无需改)。
- **CPU 侧无缺口**(11.25 的猜测不成立):#582 重写后 SW 彩色字形
  不走图集——FillGlyphs(sw_canvas.cc:584)每字形 PrepareOwnedImage
  → DrawImage,Pixmap 未打标签=sRGB 契约(对字体位图正确),经
  PixmapBrush→BitmapSampler(bitmap 空间,sw_canvas.cc:805)自动
  转换到 target。CPU 文本其余(A8 路径 raster)是 coverage 无空间。

**用户决策**:ColorEmoji(kColor/kColorSwizzleRB)支持和 CPU 后端
文字绘制都先不管 → 整个彩色图集 decode PR 无限期搁置。已确认
CPU 无回归风险,搁置无已知错误留网。

后续队列转向 #571(example)/#572(stops)适配新 #570 模型。

### 11.34 #571 语义 rebase 完成(2026-11-17)

分支重建于 main(丢弃被链路取代的 v1 基底提交),重放 3 个内容提交:
- 89370236(golden 注释修正)**跳过**——#570 重建 golden 时已吸收(240/49/28)。
- 25d0799f+3cc0bbb4(showcase+面板拆分):按 shared_ptr API 适配重放,
  与旧分支终态**逐字节一致**(旧分支后期提交早已把 example 适配到
  shared_ptr——cf4a49e2 旧草稿 rework 就含该转换)。
- 4617f270(Metal layer sRGB):clean cherry-pick。

验证:color_space_example 构建过(cmake_host_build);format 修 1 处折行
后门禁过;example/ 在 CPPLINT.cfg 排除。diff vs main = 6 文件,与原 PR
内容一致。push dev=0007b88a,PR 正文去掉 stack 说明。

### 11.35 #572 适配设计定稿(2026-11-17)

旧分支 4 内容提交 vs main 现状逐项核对后的重做方案:

**Commit 1 渐变 stop 空间(工厂重载,替代旧 sticky SetColorSpace)**:
- C++:4 个 Shader::Make* 尾部加 `shared_ptr<const ColorSpace>
  color_space = nullptr`;GradientInfo 加 color_space 成员;子类 ctor
  透传进 info_;CopyInfo 带出;FlattenToBuffer 写 kHasColorSpace_GSF +
  16 floats(!ColorSpaceIsSRGB 时)。3762e9f4(plain member 重构)随之
  作废——工厂形态下无 setter 可言。
- 解析侧:wgx_utils.cc:815 / sw_canvas.cc:747 两处改为
  Resolve(info.color_space, target) + IsNoop 守卫(未声明时与 main
  今日行为逐字节一致)。
- C ABI:**新 4 个 create 入口** `skity_shader_create_{linear,radial,
  sweep,two_point_conical}2`(尾部 const skity_color_space_info*,NULL=
  sRGB),遵循 skity_image_create_promise2 的 _2 演进惯例。不加 setter
  ——C++ 已无 setter,C ABI 镜像创建时声明契约。
- io:core_bridge::Make* 加 space 参数走 _2;shader_flat 读侧 16 floats
  → MakeRGB(失败保持 null)→ 创建时传入。
- CPU OnFilterColor 签名不动(CORE-05 CPU 半边维持延后)。

**Commit 2 blend filter 声明(GPU 侧)**:ColorFilters::Blend(Color4f,
mode, space=nullptr);BlendColorFilter 记 Color4f+space(internal
GetColor4f/GetColorSpace);wgx_filter resolve(recorded→target,legacy
Color 重载=nullptr 与 main 今日 CORE-05 行为等价);序列化写侧转 sRGB
(wire 不变,读侧零改)。CPU 半边维持 sRGB 解释(延后项)。

**Commit 3 AdobeRGB**:Kind::kAdobeRGB+工厂,gamut 必须 **D50 适配**
(旧 D65 常量违规)——用 GamutFromChromaticities 算出 D50 常量硬编码,
注释仿 P3;C ABI kind 加 SKITY_COLOR_SPACE_ADOBE_RGB(两个转换helper
同步);example 行+golden 两方向。

**Commit 4 web**:7 用例+基线。GeometryCase.target_space 值类型→
shared_ptr;main.cc 经 surface_desc.render_options.color_space。
基线 SKITY_UPDATE_MISSING_GOLDEN 重生成,按 §10.1(渐变 stop 中间可能
负分量)逐张核对。

**已知重叠(不重做)**:main 的 stop 解析机制、CORE-05 GPU sRGB 常量
resolve、web ColorSpaceDecode 用例、golden Gamma22TargetSpace
ParametricEncode 均已在链路中;#572 只加"声明"侧。

### 11.36 #572 重做完成(2026-11-17)

按 §11.35 方案在 feat-color-space-example 之上重建 4 提交:

- **C1 31bbd138 渐变 stop 空间(工厂重载)**:4 工厂尾部
  `shared_ptr<const ColorSpace> = nullptr`;GradientInfo 带 space;
  Flatten kHasColorSpace_GSF + 16 floats;C ABI `_2`×4(遵循
  skity_image_create_promise2 惯例);io 读侧 MakeRGB 恢复(失败降级
  未声明);shared helper `color_space_c_util.hpp`(bitmap_c 与 shader_c
  共用,AdobeRGB kind 一处加)。resolve 两点(wgx_utils/sw_canvas)改
  Resolve(declared, target) + IsNoop。
- **C2 0308eb34 blend filter 声明**:Blend(Color4f, mode, space=nullptr);
  BlendColorFilter 记 Color4f+space(pm_color_ 仅 SKITY_CPU,显式 sRGB
  量化注释=延后契约);wgx_filter resolve(recorded→target);Flatten 写
  sRGB 转换值 + **通道 clamp**(实测 Apply 不钳制——负值上 wire 无意义,
  q() 两端一致)。CPU 半边维持延后。
- **C3 258e077e AdobeRGB**:D50 常量(Python 复算,行和=D50);
  Kind 追加在**尾部**(kParametric=3 不漂移,C 枚举同理 —— 先插中间
  后改回);单测锚定 GamutFromChromaticities 独立推导+原色还原(注意:
  D50 矩阵列比≠原色,须 NativeChromaticities 反适配);**旧断言
  "AdobeRGB 绿比 P3 更出域"不成立**(g 通道 P3=1.018 > Adobe=1.000),
  改绝对出域断言。
- **C4 1b5fb0ce web**:7 用例+基线(SwiftShader 重生成);11 项像素
  数值核对全过(独立 CPU 数学;54/55、126/127 为 SwiftShader float
  舍入,容差内)。

红绿:C1 golden 去 declaration 挂/恢复绿;读侧注释 MakeRGB 挂/绿;
C2 去 space 挂/绿(首轮红色原色测试空转——出域钳成 (255,0,0) 两侧一致,
改域内色 (0.7,0.4,0.2) 后成立)。

验证:unit 1199/1199、golden-shape 294/294(21 个 ColorSpaceGolden
全绿)、golden-text 11/11、web 18/18(旧 11 零回归);unified dylib
导出 _2 入口(BUS-4 通配自动覆盖);format+cpplint(CI 口径
tools/cpplint_check.py)过。

**流程教训(二次踩坑)**:`git branch <name>` 只捕获提交不捕获工作树
——format --fix 后的未提交修复两次差点被丢(先 stash 拯救,后改用
/tmp 拷贝工作树文件)。另:cherry-pick 重建时基底必须是
feat-color-space-example 而非 main(#571 提交在链上);local main 在
错误重建中被移动过,已恢复 origin/main。

### 11.37 #571/#572 平衡重排(2026-11-17)

**用户问题**:#571 纯 example 改动 → CI 部分任务(path filter)不触
发;#572 又碰 example。合并 or 挪动?

**决策:平衡方案**——把 AdobeRGB 切片整体下移到 #571(理由:维持小
步 PR 哲学;核查 #572 剩余内容 zero 依赖 AdobeRGB——web 用例只用
P3/linear/gamma22;双向修 CI:#571 得 src,#572 去 example)。

新结构:
- **#571 257dd4d9**(4 提交):example×3 + "add the Adobe RGB (1998)
  color space"(工厂 + C 枚举尾加 + bitmap_c **本地** helper 加 case
  ——不引入共享 header,提取留给 #572;example 行 + 单测锚定 +
  golden 2 用例)。golden 文件 = main 版 + AdobeRGB 两用例尾部追加。
  标题改 "feat(color): add the Adobe RGB space with a color space
  showcase example"。
- **#572 17d43fe8**(3 提交):C1' gradient(+helper 提取进
  color_space_c_util.hpp,含 AdobeRGB case)/ C2 blend / C3 web。
  **新旧 tip 树零 diff**(内容全保留,仅提交结构变)。

验证:#571 树 unit 1192/1192 + golden 290/290 + text 11/11(数量与
切片吻合:1199-7=1192,#572 的 4 gradient+3 blend 单测随树走);
#572 树即已全量验证过的终态(1199/294/11/18);双分支 format +
CI 口径 cpplint 过。已 push + 双 PR 正文更新。

**教训**:`git checkout -B <branch> <base>` 后立刻用该分支名做
`git show` 源会拿到 base 内容而非旧分支——重置前先记下旧 tip hash
(1b5fb0ce)或用临时 ref。

### 11.38 #571 补 AdobeRGBTargetResolvesSRGBImage golden(2026-11-17)

用户问"需要增加对应的 golden 测试吗"→ 缺口分析:

**已覆盖**(随切片在 #571):decode 方向、AdobeRGB target 的图像直通
+ sRGB paint 解析、gamut 锚定、C 往返;codec encode 往返已用
gamma2.2+P3 参数化(AdobeRGB 仅换原色)。**不需要**:基线图 golden
(套件 21 用例仅 1 个用图,数值断言是惯例)、stops/saveLayer(机制级
已覆盖)。

**唯一缺口**:sRGB**图像**→AdobeRGB target——GPU 纹理解码管线首次
同时激活 gamut+parametric encode(P3 target 是 sRGB 曲线 encode,
gamma22 target 同 gamut;AdobeRGB 是唯一双开的内建空间;现有跨空间
target 测试只有 paint 路径)。用户确认补到 #571。

实现:sRGB 绿(0,1,0) 全幅图 → AdobeRGB target,期望 CPU Convert
独立计算(**选色讲究**:中性灰 sRGB↔gamma2.2 近恒等,直通与转换只差
1/255 无判别力;绿色 ≈(178,255,19) vs 直通 (0,255,0) 强判别)。
红绿:临时把 WGSLTextureFragment 的 Resolve(image space, target) 改
为忽略 target → 挂;恢复 → 过。

**连锁重排**:#571 amend 进 AdobeRGB 提交(b945ce92,golden 290→291);
#572 基座变化重建 3 提交(1a39a87a,残差 vs 旧 tip 恰好=新测试,
unit 1199/1199 + golden 295/295 复验)。双 PR 正文数字更新。

**zsh 教训**:`git show $OLD:test/x.cc` 中 `$OLD:t` 是 zsh 的 tail
修饰符(→"17d43fe8est/..."),golden 文件被重定向截空后 commit——
ref:path 一律**加引号** `"$OLD:$f"` 修复。

### 11.39 #571 合入 + #572 rebase(2026-11-17)

#571 squash 合入 = 0551dd96(基底 d8902cdf 之上无其他提交,树哈希
与 b945ce92 相同 d612e936 = 零调整 squash)。

#572 rebase:直接 `git rebase origin/main` 会把 #571 的 4 个原始提交
也当分支历史重放 → 与 squash 内容冲突。正确姿势 `git rebase --onto
origin/main b945ce92 feat-color-space-stops` 只重放 3 个自有提交,
**零冲突**,新链 a63aa4b9/45684d3e/4cdbf3e6,树与 rebase 前零 diff。
复验 unit 1199/1199 + golden 295/295 + text 11/11,门禁过,已 push,
PR 正文沿用。例本地运行截图核对符合预期(§11.38 后:四行色板中
linear 行≡sRGB 行为 0/1 transfer 不动点;红/蓝各行相同因 Adobe/P3
红蓝原色同 sRGB 或出域 clamp;差异只在含绿分量色块)。

### 11.40 #572 Options 机制重构(2026-11-17)

用户回顾"工厂尾部加默认参数不好"→ 方案讨论(§11.39 时的 A/B/C/D)
→ **用户决策:引入 Options,但旧签名不动,只新增带 Options 的重载**。

实现(重建 #572 三提交,c9d0ac3b):
- C++:`GradientOptions{tile_mode=kClamp, flags=0, color_space=nullptr}`
  命名空间级结构体(shader.hpp,SamplingOptions 同风格);4 工厂各加
  `(..., const GradientOptions&)` 重载,**旧签名还原为 main 原样**
  (撤掉上一版加的尾参);shader.cc 内部抽 *Full 辅助(全参版),
  两套公共入口都委托。TileMode 是 enum class → 新旧重载无歧义,
  旧调用零破坏。
- C ABI:`_2` 入口**重定义**(未发布窗口)尾部为
  `const skity_gradient_options*`(C 结构 tile_mode/flags/
  color_space 指针,NULL 入参=全默认)——单一扩展点,避免未来
  `_3`/`_4` 逐属性翻倍;GradientOptionsOf 解包。
- io/core_bridge:签名不变(尾部 space 默认参),内部组装 C options。
- 调用点迁移 8 处(ut 3+golden 3+web 1+C smoke 1):
  C++17 无指定初始化器,用位置式聚合 `{kClamp, 0, P3}`(三成员
  尚可读)。
- 残差 vs rework 前 tip = 恰好 Options 相关 9 文件;渲染值零变化
  (web 18/18 基线原样通过)。

验证:unit 1199/1199 + golden 295/295 + text 11/11 + web 18/18 +
format/cpplint 过;已 push,PR 正文更新(描述改为 options 重载)。

**流程**:#571 已合入(0551dd96)后本次直接在 origin/main 上重建;
cherry-pick 链 + 在链上重跑 format --fix(输出确定性,等价于备份
文件法——备份 /tmp/fmt 曾被 rm 误删,format 工具重跑补救)。

### 11.41 ImageFilter 颜色空间盘点 + DropShadow 决策(2026-11-17)

用户问"是不是还没处理 ImageFilter"→ 全路径核查:

**按设计无需处理**:空间类操作(blur/morphology/offset/matrix/
compose/merge)操作已 target 编码像素,输出同空间;saveLayer 层带
target tag(#570)。

**DropShadow 阴影色缺口**(唯一带颜色常量的 ImageFilter):
- GPU:hw_filters.cc:207 组装为 `ColorFilters::Blend(color,kSrcIn)` →
  **搭 #570 CORE-05+#572 顺风车**,bind 时 sRGB→target 已正确
  (副产品,非显式设计);但无声明 API,只能 sRGB 约定;
- SW:image_filter.cc:244 `ColorSetA(color_,a)` 按字节染色零转换
  (CORE-05 CPU 延后家族);
- 序列化(local-matrix 复制/Flatten)颜色原样,sRGB 约定一致。

**用户决策**:SW 等整体重构;**GPU 声明 API 放下一个 PR**。

**下 PR 工作项**(#572 合入后开,分支挂其后):
1. API:`ImageFilters::DropShadow` 加 Color4f+space 重载(Color 整型
   vs Color4f 聚合,无重载歧义;镜像 #572 Blend 形态);旧签名不动;
2. DropShadowImageFilter 记 Color4f+color_space_;SW OnFilter 维持
   sRGB 字节量化(延后契约,注释同 BlendColorFilter);
3. **wire 零改动**:FlattenToBuffer 本就序列化为 Blend(kSrcIn) 组合
   图,改用 `ColorFilters::Blend(color4f, kSrcIn, space)` 后 Blend 的
   flatten 自动转 sRGB——无需新序列化代码;
4. GPU:hw_filters kDropShadow 传 GetColor4f()/GetColorSpace() 进
   Blend 重载,搭现有 wgx resolve;
5. local-matrix 重建路径(image_filter.cc:125)透传空间;
6. 测试:ut 记录+flatten;golden 红绿(P3 声明阴影→sRGB target;
   另 pin legacy sRGB 阴影在 P3 target 的 ride-along 正确性)。

### 11.42 #572 review 五项修复(2026-11-18)

Handoff skity-color-space-stops-c9d0ac3b-handoff,STOPS-01~05 全部
成立(评估见会话),修复已 push = 6a9d8d7d(链 bcc6fd80/ddd8a42e/
6a9d8d7d,fix 分摊 amend 进原两提交):

- **01 格式兼容(P1)**:新共享 helper `src/effect/color_space_block.hpp`
  (Skia v1:4B 版本头+7tf+9gamut=68B);渐变写读都走它;gamma filter
  写侧重构复用(消协议分叉);读侧校验 68B+版本字节。我的 64B 断言
  测试同步改。
- **02 渐变 clamp(P2)**:四个渐变类型的 shader 输出漏斗处
  `clamp(color.xyz, 0, vec3(color.w))`(插值后、×global_alpha 前)
  ——满足"不过早裁剪色标",域内值零变化(基线零漂移)。**教训:
  shader 文本里不能加注释行**(ShaderWriter 期望串是行比较,注释
  行会造成 diff;理由写 C++ 注释)。
- **03 blend clamp(P2)**:wgx_filter resolve 后 unpremul 域 clamp
  再预乘。
- **04 序列化扩展值(P2)**:写侧 Resolve+clamp 改 `ColorSpace::Convert`
  (信号值、扩展域;此前担心的"fold 出线性值"是误判——gamut 矩阵
  在中间,曲线不可折叠);读侧新增 C ABI
  `skity_color_filter_create_blend4f` 浮点通道替代 Color4fToColor
  降级。往返:P3 红在 P3 target 直绘=往返=(255,0,0)(负通道的
  encode/decode 不完全可逆,但误差全落在最终 clamp 掉的通道里)。
- **05 测试数组(P2)**:`red[2]`,两调用共用。

验证:reviewer 5 复现 5/5;回归组 81/81 单测+31/31 Metal golden;
ASAN(RelWithDebInfo,IO=OFF)Capi 测试无越界;全量 1199+300+11;
web 18/18(shader 变更后基线原样过);unified dylib 导出 blend4f;
format/cpplint 过。PR 正文已更新。

**记录**:SW 渐变 stop resolve(sw_canvas)与 GPU 同款无 clamp——
reviewer 未列、本轮未动(SW 家族统一留给渲染器重构,与 SW blend
同决策)。

### 11.43 回溯:五项为何漏过 + 固定自查单(2026-11-18)

用户问"是不是因为没有测试"→ 归因:四种失效模式,不止缺测试——

1. **自指测试**(01):roundtrip 测自己写自己读,发现不了外部不兼容;
   wire 断言反而把错误格式钉成预期;
2. **矩阵缺维度**(02/03):所有颜色用例不透明,RGB>A 只在
   半透明×宽色域暴露;
3. **测试锁死错误契约**(04):测试挂时改生产(clamp)让它过而非
   质疑期望——方向修反,之后断言钳制值;
4. **无 ASAN 惯例**(05):测试本身越界,只有 reviewer 的 ASAN 能炸出。

系统性根因:**"每个 Apply 新消费点都要回答'知道 target 吗'"**
(CS-01 建立的规则)没变成检查单——#570 paint resolve、#572 stop/
blend resolve 每轮都是新消费点,两次都没问。

**后续 PR 固定自查单**(用户确认,触碰 ColorSpace/序列化时逐条过):

- [ ] **WIRE**:新增/改 wire 格式 → 必含"读外部标准 payload"方向
  的测试(不能只自写自读);动格式前先查仓库既有同类序列化
  (68B 块统一在 src/effect/color_space_block.hpp);
- [ ] **COLOR 矩阵**:声明空间用例最小覆盖
  {宽色域原色, 域内中间色} × {不透明, **半透明**}
  (半透明×宽色域 = premul 类 bug 唯一探测器);
- [ ] **ROUNDTRIP**:序列化往返测试必须在**非 sRGB target 回放**
  (提前 clamp/sRGB 降级只有宽色域回放才暴露);
- [ ] **ASAN**:新增测试本地过一遍 ASAN
  (build-review-asan 现成:configure 见 §11.42 复现配置,一条命令);
- [ ] **APPLY 消费点**:新调用 `ColorTransformSteps::Apply`/
  `ColorSpace::Convert` 输出到渲染的位置 → 回答"此处知道 target 吗":
  知道 → 消费前 unpremul 域 clamp;不知道(序列化/中间表示) →
  保持扩展值。已知消费点台账:图像纹理(shader/sampler,已 clamp)、
  paint 纯色(域内安全)、渐变 stop(GPU 输出 clamp,SW 渲染器
  重构时补)、blend 常量(GPU 已 clamp,SW 延后)、DropShadow 阴影色
  (GPU 经 Blend ride-along,SW 延后)、序列化写侧(扩展值)。

### 11.44 DropShadow 声明 PR 执行(2026-11-18,PR #587)

#572 合入(24541aa3)后按 §11.41 工作项执行,单 commit 2f58bbcc,
分支 feat-color-space-drop-shadow:

- **API**:`ImageFilters::DropShadow` Color4f+space 重载(Color 整型
  vs 聚合 + 形参个数,双重无歧义);旧签名/C ABI 入口不动(io 读侧
  也无需新通道——组合图复用 #572 的 blend4f);
- **wire 零改动验证成立**:legacy Color 经 Color4fFromColor(无损)
  + null space → Convert 恒等,字节级等价(golden 有 memcmp pin);
  声明空间的宽色域经 Blend 节点的扩展浮点存活;
- **发现(重要)**:DropShadow 的 FlattenToBuffer(嵌套 flattenable)
  **此前从未被任何测试执行过**——首次执行即在
  `BinaryWriteBuffer::WriteFlattenable → FactorySet::AddFactory`
  空指针崩溃(默认构造 buffer 的 factory_set_==nullptr)。
  测试里嵌套 flattenable 的正确姿势 = 镜像 record_playback:
  `FactorySet fs; writer.SetFactorySet(&fs); ... reader.SetFactorySet(&fs)`
  (同一集合,读侧 factory_count>0 走索引模式)。stops_review 的
  blend/gradient 没踩到是因其 payload 无嵌套 flattenable;
- **测试(按 §11.43 单)**:WIRE=真实 reader 往返 + legacy 字节 pin;
  COLOR 矩阵={P3红,域内mid}×{不透明,半透明} vs 图像参照路径
  (阴影区域内部 maskA=1 → 像素=解析后颜色,可与图像逐像素对齐);
  ROUNDTRIP=P3 target 回放 direct=(255,0,0) pin;ASAN=ut 4/4
  (build-review-asan)+ golden 含 IO 4/4(新建 build-asan-golden,
  RelWithDebInfo+IO=ON+GOLDEN=ON——§11.42 的 asan 目录 IO=OFF
  装不下 roundtrip 用例);
- **验证**:ut 85/85(#572 回归组+ImageFilter)、golden 42/42
  (ImageFilterGolden+ColorSpace 全家)、ASAN 双配置全绿、
  format/cpplint 过;
- **台账更新(§11.43)**:DropShadow 阴影色 GPU 从"ride-along 副产品"
  升级为"显式声明,bind 时经 Blend resolve+clamp";SW 字节染色延后
  契约注释与 BlendColorFilter 同款。

遗留:SW 侧(渲染器重构吸收);web harness 未加 DropShadow 用例
(不在 §11.41 工作项,保持 PR 聚焦)。

### 11.45 DropShadow Options 化(2026-11-18,#587 开放期)

用户问"ImageFilter 能否也引入 Options 机制避免工厂函数参数过多"
→ #587 未合入,直接在开放 PR 上重构(镜像 GradientOptions 模式):

- `struct DropShadowOptions { color_space; Rect crop_rect = {}; }`
  放 image_filter.hpp 命名空间作用域;**用户选定 crop 收进 Options**;
- 新重载 7 参尾默认:`DropShadow(dx,dy,sx,sy,Color4f,input,
  const DropShadowOptions& = {})`,调用 `{P3}` 即声明;
  Color 整型 vs Color4f 聚合 + 形参个数,双重无歧义;
- **不做**家族级 ImageFilterOptions:Blur/Dilate/Erode 只有 2 参,
  家族内工厂形参形状各异,共享 struct 是投机设计;按需 per-family
  (Gradient 先例:一个 struct 服务同形工厂族);
- crop_rect 是惰性成员(存而不用,无访问器)——不为测试硬造断言,
  Options 默认值断言(null space)代替;
- 1aad028b force-push,ut 4/4+回归 81/81+golden 7/7+ASAN 双配置
  (ut 4/4、golden 含 IO 4/4)、format/cpplint 过;PR 正文已更新。

**模式沉淀**:声明空间的入口统一收敛为"Options 尾参"形态——
GradientOptions(#572)、DropShadowOptions(#587);后续若 Blur 等
需要限定参数,新增 BlurOptions 而非改签名。

### 11.46 #587 自查审计(2026-11-18,用户触发)

用户要求按既往经验查缺补漏(测试/破坏面/序列化)。逐项过 §11.43 单
+ STOPS 教训,结论与补强(597d34b9):

**排查过、确认无问题的**:
- **SW 编译覆盖**:`SKITY_CPU` 由 `SKITY_SW_RENDERER` 门控,查
  CMakeCache:三个本地构建均 ON → SW OnFilter 量化提升代码一直有
  编译;SW 语义零变化(Color4fToColor(Color4fFromColor(c))==c);
  SW 运行时本就无测试(CPU 渲染器重构家族,先例如此);
- **嵌套读取路径**:Common::ReadCommon → buffer.ReadImageFilter()
  → 索引→factory 名→proc 全链,往返测试已覆盖嵌套 blur/color filter
  节点的 proc 解析,不是只测顶层;
- **爆炸半径**:HWFilters::DropShadow/GetColor 的调用方全仓 grep,
  仅 hw_filters 与 local-matrix 两处(都已改);example 用 legacy
  Color 调用(filters_c.cpp 同),公开头是平台无关 C++17,四个 CI
  平台(mac/ubuntu/iOS/Android/emscripten)编译面一致;
- **wire 兼容方向**:picture_priv V100 注释证实 Skia≥v100 的
  DropShadow 本就序列化为组合图(无专属实现)——组合图恰是现代
  外部标准形态,我们的写读两侧都在该形态上,节点级格式均有 #570/
  #572 的外部 payload 测试兜底;
- **ReadCommon 的 legacy crop 字段**:IsVersionLT 在 version=0/modern
  时跳过,与 writer(不写 crop)匹配,既有行为未动;
- **skp golden fixtures**:不涉及 image filter;外部 SKP 回放面
  wire 未变。

**补强的**:
- legacy LocalMatrix 重建 pin(Color4f 往返经重建路径 GetColor()
  ==Color_RED、space null)——防"Color→Color4f 存储化"在重建
  路径上引入量化漂移;
- Options 重构后全集复跑:ut 4/4+81/81 回归、golden 46/46
  (ImageFilterGolden+ColorSpace 全家)、ASAN ut 4/4、format/
  cpplint 过,597d34b9 已 push。

**已知边界(记录,不算缺口)**:
- crop_rect 序列化丢弃(FlattenToBuffer 从不写 crop——先于本 PR
  的既有行为,Options 只是透传到 ctor,与 legacy 一致);
- 带 input 的 DropShadow 序列化往返无测试(改动前亦无,input_
  处理未动);
- sRGB target 回放宽色域声明阴影(扩展浮点×sRGB 解析)未单测:
  与 direct 声明绘制在 sRGB target 收敛到相同 clamp 像素,且
  reviewer 先例(#572 blend)只测 P3 回放;P3 回放已覆盖
  "提前降级"探测器。

### 11.47 C ABI ColorSpace 补齐(2026-11-18,PR #588)

#587 合入(0d379e00)后,用户要求补齐 capi 的 colorspace 能力。
盘点:C ABI 已有渐变声明(shader_create_*2)、图像空间
(pixmap_set_color_space)、色域数学;缺 6 项,最致命是 surface
无法声明目标空间(宽色域链路 C 侧完全进不去)。

**新增 6 组入口**(全部新符号,旧签名零改动;c42a2b4f):
1. `skity_surface_render_options` p_next 扩展(新 structure type),
   GL/VK/MTL 三路统一注入 GPUSurfaceRenderOptions——
   **顺带修了 Metal glue 硬 cast 首个 p_next 的隐患**(render
   options 在链首会错读):新增共享 surface_pnext.hpp 前缀视图
   链遍历,backend 节点查找也改链感知;
2. `skity_color_space_info_init(kind)`(命名空间规范值,PARAMETRIC
   拒绝)+ `_init_parametric(tf,gamut)`(MakeRGB 同款校验);
3. `skity_paint_set_color4f`(void,与兄弟 setter 一致);
4. `skity_color_filter_create_blend4f2(color,mode,space)`(blend4f 保留);
5. `skity_drop_shadow_options{color_space,crop_rect}` +
   `skity_image_filter_create_drop_shadow2`;
6. `skity_color_space_convert`(纯数学,NULL=sRGB,扩展值保留;
   skity_vec4 union 不能从 Color4f 赋值——memcpy)。

**验证**:Capi 全家 48/48;ASAN(RelWithDebInfo)11/11;回归组 46/46;
unified dylib 6 符号导出确认(nm);Metal glue 已编译(MTL=ON);
format/cpplint 过。

**记录**:全量单测 1223/1225,失败的 2 个是 Arena.AllocateZeroByte/
NotAlignedToPowersOfTwo(死亡测试 failed to die)——stash 复测证明
main 纯净状态同样失败,**与本 PR 无关**的既有环境问题(§11.42
全量通过后出现,疑本地工具链漂移;CI 有结果后核对)。

**模式**:C ABI 声明空间入口与 C++ 收敛为同构面——
Options 尾参(C)/Options 结构体(C++)/info POD 承载空间;
命名沿用 *2 后缀与 p_next 扩展惯例。

## 12. 一期收官盘点(2026-11-19,main = 8d58f716)

用户问"ColorSpace 是否可以告一段落"→ 盘点结论:**可以**。
GPU 全链 + 序列化互操作 + C ABI 三层已闭环。

### 12.1 已合入链(9 个 PR,按序)

| PR | 内容 |
|---|---|
| #555 | shared_ptr 引用语义(interning、null=sRGB 可观测) |
| #575 | 色度数学核心层(Bradford、D50 约定) |
| #579 | GPU 纹理注入(MakeHWImage/GetColorSpace) |
| #580/#581 | codec 解码色彩标记(PNG cHRM/iCCP/sRGB/gAMA) |
| #570 | per-target 工作空间模型(paint/图像/saveLayer/surface) |
| #571 | AdobeRGB + showcase example + Metal sRGB layer |
| #572 | 渐变 stop + blend 声明;Skia v1 wire;clamp 修复;web harness |
| #587 | DropShadow 声明(DropShadowOptions 形态) |
| #588 | C ABI 补齐(surface 目标空间/info 构造/声明入口/convert) |

§8"明确剩余"四项中两项已随链完成:渐变插值空间(#572 目标空间
插值语义)、Blend 空间序列化(#572 扩展浮点);§2.8"C API surface
空间参数"由 #588 完成。

### 12.2 能力矩阵(现状)

- **目标声明**:GPU surface(render_options)+ web harness + C ABI;
- **源声明**:paint 纯色、图像解码、渐变 stop、blend 常量、阴影色
  (C++ 全部 + C ABI 同构面);
- **消费点**:全部过 §11.43 APPLY 台账(GPU clamp/序列化扩展值/
  SW 延后契约);
- **互操作**:Skia v1 68B 块统一(color_space_block.hpp),
  组合图 DropShadow 与外部现代 payload 同形;
- **测试**:COLOR 矩阵/ROUNDTRIP(P3 回放)/ASAN/WIRE 四类模式
  全覆盖,基线与独立 CPU 数学对账。

### 12.3 遗留清单(重启指针)

**A. SW/CPU 渲染器重构家族吸收**(用户决策,等整体重构):
SW blend filter 转换、SW DropShadow 色、SW 渐变 stop clamp、
CORE-01(SW NEON nearest)、CORE-03(SW saveLayer)、CORE-05 CPU。
重启时逐一对照 §11.43 台账补 SW 消费点 clamp。

**B. 边角项**(低频,可随时单独小 PR):
顶点(Vertices)颜色空间、snapshot 类 API 的 target 空间参数
(现固定 sRGB)、WebP/JPEG ICC 解析、web harness DropShadow 用例
(§11.44)。

**C. 第 4 期(§9,显式不在本期)**:ICC/CLUT vendor 重评、
PNG cICP、fp16 线性目标、sRGB 硬件附件、HDR/PQ、
ColorEmoji 彩色字形、CPU 后端文字。

**环境备忘**:Arena 2 例死亡测试本地失败(main 纯净态复现,
非 ColorSpace 家族;CI 侧正常则纯属本地工具链漂移,§11.47)。

任何重启者:先读 §2(定死前提)与 §11.43(固定自查单)。
