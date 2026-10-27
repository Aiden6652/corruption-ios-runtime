# corruption-ios-runtime

在 **non-jailbroken iOS (arm64, JIT 可用)** 上运行闭源 x86_64 Windows exe 的运行时：
FNF *Corruption* mod（基于 **Psych Engine 0.6.3** 深度重做，HaxeFlixel/Lime/OpenFL 引擎）。

路线：**用户态 x86_64 → arm64 JIT 翻译 + Win32/libVLC shim**，不跑整机 Windows-on-ARM。

---

## 为什么是这条路
- exe 是 **x86_64 原生 PE**（`0x8664`，PE32+，38MB），只 import 4 个 DLL：
  `KERNEL32`(141)、`ADVAPI32`(5 注册表)、`USER32`(仅 MessageBoxA)、`libvlc.dll`(72)。
  **没有 GL/D3D/GDI/winmm 导入** → 渲染很可能以运行时 `LoadLibrary` 的额外引擎 DLL 形式存在（见“已知缺口”）。
- 源码原生移植已证伪：作者本人干了 3 个月，死在“加载不了原生资源”。
  根因几乎可以肯定是 **文件系统大小写敏感 + 路径分隔符 + 工作目录** 差异
  （Windows 大小写不敏感、`\`；iOS 敏感、`/`）。本运行时用 `path_resolve` 在模拟层
  **完全复现 Windows 路径语义**，把这个坑填平（算法已用 Python 原型验证：PASS）。

## 架构
```
  iOS App (Swift, Metal 视图, JIT entitlement)
        │ 实时线程
        ▼
  app_glue.c ──► emu_create / emu_load_exe / emu_warmup / emu_run
        │
        ▼
  Unicorn (x86_64 → arm64 TCG 翻译)
     ├─ AOT 预热：先把静态代码段翻译进缓存，首关不卡
     ├─ JIT 兜底：未命中的块/动态生成代码现场翻译
     └─ import trampoline 挂钩：guest CALL [IAT] → 主机 shim
        │
        ├─► Win32 shim (KERNEL32/ADVAPI32/USER32)  ── 文件IO 全走 path_resolve
        └─► libVLC shim (72 个 libvlc_*) ──► MobileVLCKit（音视频原生，不翻译 VLC）
                                              音频走 AVAudioSession（低延迟）
                                              视频帧经 delegate → 游戏回调
  JIT 内存：mmap MAP_JIT + pthread_jit_write_protect_np（W^X 翻转）
```

## 目录
```
runtime/pe_loader.c       PE 解析 + 段映射 + IAT→trampoline
runtime/win32/path_resolve.c   大小写不敏感路径层（关键）
runtime/win32/win32_shim.c     KERNEL32/ADVAPI32/USER32 shim + 符号注册表
runtime/win32/clock.c           QPC → mach_absolute_time（稳定无漂移）
runtime/jit/jit_mem.c          W^X JIT 分配器
runtime/translator/unicorn_glue.c  Unicorn 集成 + AOT 预热 + import 桥接
runtime/libvlc_shim/vlckshim.m   72 个 libvlc_* → MobileVLCKit
ios/                    Swift 应用层（Metal 视图、JIT 获取、模拟器线程）
tools/path_resolve_proto.py  路径解析 Python 原型（已跑通 PASS）
docs/imports.txt         exe 完整 import 清单
```

## 构建（需要 Mac + Xcode）
1. `brew install xcodegen`；`pod 'MobileVLCKit'` 或下载 MobileVLCKit .framework。
2. 编译 Unicorn 为 iOS arm64 静态库，放到 `unicorn/prebuilt/libunicorn.a`
   （Unicorn 官方支持 iOS：`CFLAGS=-target arm64-apple-ios ...`）。
3. `xcodegen generate` → 打开 `CorruptionIOS.xcodeproj`。
4. 把 `Corruption.exe` 及其 assets 目录拖进 App 的 Copy Bundle Resources。
5. 用 **开发签名 + JIT entitlement**，经 AltStore/SideStore「启用 JIT」或 Xcode 调试启动。
6. 真机运行。

## 已知缺口（按优先级）
1. **动态加载的引擎 DLL**：主 exe import 极简，真正的渲染/逻辑模块大概率在运行时
   `LoadLibrary` 进来。需让 `LoadLibrary` 同样加载并翻译这些 x86_64 DLL
   （复用 pe_loader + trampoline 机制）。
2. **渲染桥接**：游戏的可见帧如何产出（libVLC 视频回调 / 软件帧缓冲 / 额外 DLL 的 GL）
   待确认后对接 Metal 视图（见 `GameViewController.draw` TODO）。
3. **磁盘持久化 AOT 缓存**：当前 AOT 是“启动预热”（同一次运行内有效）。真正的跨启动
   缓存需序列化 Unicorn 翻译块，或换用支持翻译序列化的 Unicorn fork。
4. **延迟调优**：音频/输入/帧节奏三件套需在真机用探针量化（目标：音频 <20–30ms、
   帧抖动 <2ms），再针对性优化。

## CI / GitHub Actions
公开仓库 => macOS runner 分钟数无限。代码进仓后自动完成：
`编译 Unicorn(iOS arm64)` → `pod install (MobileVLCKit)` → `xcodegen` → `xcodebuild`（无签名，验证编译+链接）。

- 工作流：`.github/workflows/ci.yml`
- 触发：push 到 main/master、PR、或手动 `workflow_dispatch`
- 产物：每次构建的 `build.log` 作为 artifact 上传，回来看编译错误/红绿
- 真机签名**不在此 CI 范围**（需本地证书 / AltStore 启用 JIT）；CI 只保证 C/ObjC/Swift 能过编译链接
- Unicorn 无 iOS 二进制发行，CI 里 `./make.sh iOS` 现场交叉编出 `libunicorn.a` 放到 `unicorn/prebuilt/`

## 状态
- ✅ 路径解析算法（资源加载失败的根因）已验证
- ✅ PE 加载器 / Win32 shim / libVLC→MobileVLCKit / JIT 分配器 / Unicorn 胶水 —— 已落地（待 Mac 编译验证）
- ⏳ 动态 DLL 加载、渲染桥接、延迟调优 —— 待迭代
