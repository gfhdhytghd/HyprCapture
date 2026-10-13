# HDR 延迟调查：phase 1

**直接 FP16 可行，但必须走 dmabuf；当前 Hyprland 不支持 FP16 shm。** 独立客户端已实际显示 Windows-scRGB 与 parametric sRGB/ext_linear FP16 表面，并收到真实呈现反馈。滚动入口尚未完成桌面归因；离线数据和用户运行的计时脚本已准备好。本阶段只增加诊断，未接入新显示路径。

2026-10-13；Intel Arc 140V / Core Ultra 7 258V，Hyprland 0.56.2，eDP-1 2880×1800、scale 2、59.99 Hz、HDR/XRGB2101010。Release，AC/performance，无编译进程；每项 5 次，下表为后 4 次中位数。CPU 使用实际产品代码，GPU 每个边界 `glFinish`；数据是局部操作耗时，**不是启动或显示延迟**。完整环境、源码/二进制标识见 [environment.json](performance/hdr-latency/environment.json)，范围/首次调用值见 [summary.json](performance/hdr-latency/summary.json)。

**A 的测量与可行性。**

| 操作（ms） | 2880×1800 | 6144×3456 |
| --- | ---: | ---: |
| CPU scRGB → PQ10，4 workers | 32.83 | 122.64 |
| PQ10 memfd 映射、复制、释放 | 8.44 | 34.29 |
| GPU FP16 上传并等待 | 2.37 | 34.92 |
| GPU FP16 复制并等待 | 0.50 | 1.59 |
| GPU PQ shader 并等待 | 0.49 | 2.28 |
| GPU PQ10 读回 CPU | 1.52 | 13.46 |

[CPU CSV](performance/hdr-latency/ui.csv)、[GPU CSV](performance/hdr-latency/gpu.csv)。CPU SDR preview 仍需约 21.62 / 87.43 ms。另一轮工作集布局曾测得 CPU PQ **123.26 / 514.66 ms**，差异原因尚未确定；[保留数据与说明](performance/hdr-latency/cpu-pq-earlier.md)。两批不合并，也不归为产品优化收益。早期并发编译数据标为 provisional，不进入最终统计。

运行中合成器公布的 [完整格式、CM features/TFs/primaries/intents](performance/hdr-latency/capabilities.log) 表明：shm 只有八种 8/10-bit 格式，没有 16F；dmabuf 公布 `ABGR16161616F`（AB4H），包括 LINEAR modifier；CM 公布 parametric、windows_scrgb、ext_linear、sRGB、PQ/BT2020，intent 为 perceptual。

[`fp16-probe`](../tools/hdr-latency/fp16-probe.cpp) 只提交已公布格式。Windows-scRGB 与 parametric（sRGB、ext_linear、80-nit normalization）两条路径均得到 READY，实际显示 512×256 灰阶/高亮/负通道测试窗，`presented=1, display_error=0`；attach→呈现反馈约 26.68 / 34.49 ms，见 [两个最终日志](performance/hdr-latency/fp16-windows-scrgb-final.log)、[parametric 日志](performance/hdr-latency/fp16-parametric-final.log)。单次小窗数据不用于比较描述速度。

Intel GBM CPU map 的早期尝试失败；EGL 导入 BO、上传原始 FP16 并复制成功。GPU 离线检查保留负值、超过 1 的值和 alpha，FP16 逐位相同；六组 PQ 色块采样与 CPU 相差 0 个 10-bit 码值，见 [检查日志](performance/hdr-latency/gpu-stderr.log)。**已证明传输/呈现，未证明面板实际亮度、色域或两种描述颜色等价。**

源码核查直接读取本机 git 的 **0.56.2 commit `efb509937…`**，避免用较新 checkout HEAD 代替：Shm 固定格式名单并拒绝未公布格式；GLTexture 按 DRM 格式上传；ColorManagement 实现 Windows-scRGB 描述；CM shader 保留 extended-linear 值。准确行号和摘录见 [source-evidence.md](performance/hdr-latency/source-evidence.md)。结论是 shm FP16 不可用、dmabuf FP16 可用，无需先修改 Hyprland。

**B 的离线分解。** 同一 fixture 对比 SDR 单平面和 SDR+FP16 双平面；隔离传输/处理成本，不代表显示器 SDR/HDR 端到端 A/B。

| 2880×1800 操作（ms） | SDR 单平面 | SDR+HDR |
| --- | ---: | ---: |
| PBO read + finish | 0.63 | 3.01 |
| PBO map + CPU copy | 0.88 | 2.45 |
| memfd write + seal + close | 5.43 | 14.71 |
| 基线 append | 1.00 | 0.84 |
| 基线预览生成 | 1.24 | 2.11 |
| 移动 1/4 视口后的 append | 33.68 | 40.87 |
| 移动后的预览生成 | 0.84 | 1.90 |

HDR 接收映射 0.028 ms；HDR 缩略图 PQ 3.67 ms，不含 Wayland 上传。FP16 底图 QPainter 合成 7.29 ms、无标注 editor HDR renderer 2.33 ms、live image 安装约 0.003 ms。首次 HDR PBO 分配 3.77 ms，后续复用约 0.001 ms；不能只用 warm 数据排除冷分配。

每帧传输从 19.78 MiB 增到 **59.33 MiB**，双平面 read/map/publish 成本明显增加，基线 append 没有明显 HDR 增量。这些阶段跨线程，不能相加当作入口延迟。6K 底图合成 / editor HDR renderer 为 26.47 / 13.24 ms；6K HDR 双平面 **243 MiB** 超过 128 MiB transport 和 16M HDR stitcher 上限，源码会拒绝，基准已验证。6K GPU 数据仅作假设扩容成本，不冒充可工作的长截图测量。

源码确认窗口基线会预热，区域基线等首次输入才捕获；常规 timer 33 ms，输入可提前唤醒。进入编辑器会同步 `renderResultImage(true)`，后台 HDR 编辑预览可能继续竞争 CPU；开始滚动时隐藏 backdrop。worker 无条件生成 HDR preview，HEIC 缩略图在 UI `present()` 同步 PQ 转换。**这些是已证实路径，尚不能认定哪个是桌面停顿主因。**

诊断现已覆盖编辑底图/后台 HDR render-convert-upload、input/baseline ack、逐序号 capture/readback/submit/receive、append/previews、UI 队列/present/Paint、PBO allocate/read/map/copy；UI 保留毫秒并新增微秒。脚本按同一序号分解，避免把窗口预热算进输入后的等待。

**下一阶段建议与风险。**

1. A 优先 capability-gated FP16 dmabuf：预初始化 EGL/GBM，复用缓冲/描述；所有 HDR 子表面就绪后，与透明 Qt 控件在第一次父提交中一起显示。再评估导出 compositor 的冻结 FP16 framebuffer，避免读回和重传；保持 frozen snapshot、fd/release/fence 生命周期。旧 CPU PQ+copy 局部预算约 **41.3 / 156.9 ms**，上传+复制约 **2.9 / 36.5 ms**，这段预计可省 **38 / 120 ms**。不包含上下文、BO 分配、协议或合成，不能承诺启动加速。四纹理冷分配实验为 4.64 / 53.93 ms，首次 2880 上传 9.97 ms，必须验证冷启动无退化。
2. GPU PQ→RGB30→shm 可作后备，上传+shader+读回+copy 预算约 **12.8 / 84.9 ms**；保留 CPU fallback。颜色验收覆盖 80/203-nit 与当前 output reference white、峰值/负通道/P3、premultiplied alpha/checkerboard、scale/朝向、混合输出和取消；防止描述 metadata 引起再次色调映射。实验 shader 尚未接入产品。
3. B 先跑桌面 trace，再按占比优化：不显示时避免 HDR preview；避免过期编辑预览工作；复用 HDR 底图；传输确为主因时减少 PBO→vector→memfd 复制；区域预热需处理目标调整/输入时序。保持当前安全上限。验收需对照首帧呈现及冷/暖分布，确认无 SDR flash、无启动退化。

**桌面实测需你执行脚本。** 本阶段没有重载/卸载插件、没有修改 Hyprland 配置。初次核对的模块是临时 `hc-touchpad.so`，而非 hyprpm 缓存；结束时另一个同名二进制也出现于映射中，注册 handle 未变，脚本保存已知原模块并校验 handle/哈希。不能把本工作树构建当作现有插件实测。

请在可以短暂切换插件时，先查看 [`state.json`](/home/wilf/.cache/hyprcapture-hdr-phase1-20261013-final/state.json)、[`activate.sh`](/home/wilf/.cache/hyprcapture-hdr-phase1-20261013-final/activate.sh)、[`restore.sh`](/home/wilf/.cache/hyprcapture-hdr-phase1-20261013-final/restore.sh)，关闭覆盖层后自行运行 activate；测完自行 restore。它们保存原二进制和 helper/timing 设置，不编辑配置文件。当前状态若已变化，重新 prepare。

```sh
tools/hdr-latency/measure-live.py --out "$HOME/.cache/hyprcapture-hdr-phase1-20261013-final" --record hdr-window --seconds 30
```

录制后手动截图、选同一目标、滚动一次并取消。交替测 hdr-window、hdr-region、hdr-window-png、sdr-window、sdr-region，各至少 5 次；显示器模式由你切换。窗口分别立即滚动和等 1 秒，分离预热。记录实际二进制/显示模式及输入→首个有效缩略图 Paint 入口；**这仍不是显示扫描输出**。完整复现/恢复步骤见 [tools README](../tools/hdr-latency/README.md)。

Release plugin/UI 和独立工具构建通过；scroll-capture、scroll-stitcher、hdr-backdrop、in-place-editor **4/4 通过**，见 [tests.log](performance/hdr-latency/tests.log)。保留诊断、工具、原始数据和报告；未改变产品显示/捕获策略，未 git commit。
