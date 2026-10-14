# Phase 2B：进入编辑器的 HDR 阻塞

实现完成，未提交。没有切换在线插件/helper 或修改在线配置；Phase 1 的测量目录仅用于读取已有记录。现场 after 验收按任务要求交给用户运行脚本，不能把以下离线结果当成现场点击延迟。

## 现场基线

按每个 `manifest.json` 的 start/end 单调时钟窗口过滤累计日志，得到 8 次窗口、3 次区域样本，证据在 [baseline-live.csv](performance/hdr-phase2b/baseline-live.csv) 和 [baseline-live-events.json](performance/hdr-phase2b/baseline-live-events.json)。

窗口模式：`begin_edit` 653.8–1929.3ms；首次 SDR render 293.2–370.5ms（包含 artifact hydration/同步 compositor 捕获），第二次 HDR render 330.3–1554.9ms。区域模式对应入口为 4.9–5.2ms。日志有 CPU 竞争的上界样本；没有将累计日志中的旧 trial 重复计数。

## 修改

- SDR 结果准备好后立即建立编辑器，返回事件循环；HDR base 使用只持有冻结 QImage 和参数的后台 renderer，不访问窗口、style hints、IPC 或 Wayland 对象。SDR 输出不再计算未使用的 HDR base，切换到 HDR 时才启动。
- SDR render 完成的 hydration 直接供 HDR 快照使用，不再在 HDR worker 中重复请求 compositor 或加载 artifact。
- 刷新选择/背景时取消旧 token、递增 generation；过期结果不覆盖新 base。关闭/取消不等待后台任务；预览线程池改用全局池，去掉 QWidget 子线程池销毁时的 join。
- HDR 确认导出时若 base 尚未完成，异步延后导出，保留取消/绘制能力；完成后使用原始浮点 HDR base 和最新 annotation 生成输出，不从 SDR 或显示 backdrop 反推 HDR。
- 对齐原捕获位置且 native HDR 表面覆盖完整的编辑区域，在 base/预览准备期间继续显示冻结 HDR 桌面；刷新时保留已有 native 编辑表面。Stage/居中、平移或覆盖不完整的区域保持安全的 raster 预览，不宣称这些条件下消除了 SDR/HDR 切换。
- 原生 HDR real-background 存在时，删除先 `sdrToHdr(background)` 再清空并覆盖该结果的无效全图转换。背景 alpha/clip、前景 FP16、cursor、参考白及导出流程保持原语义。
- 增加 hydrate、dispatch、load、HDR→SDR、foreground prepare/compose、background conversion/clip/compose、cursor、HDR base 的细分计时；记录 editor ready 与首次 canvas Paint 入口。各 scope 可嵌套，不能把耗时盲目相加。

## 2386×1656 离线证据

预先 hydration 的同尺寸 HDR real-background fixture，Release、offscreen、测量时未运行编译：

| 次数 | SDR render ms | HDR render ms | 新 editor begin ms |
|---|---:|---:|---:|
| 0 | 60.974 | 56.006 | 51.750 |
| 1 | 37.448 | 52.185 | 38.473 |
| 2 | 37.456 | 48.001 | 38.442 |
| 3 | 43.756 | 48.341 | 40.494 |
| 4 | 39.988 | 46.318 | 38.026 |

中位数为 SDR 40.0ms、HDR 48.3ms、editor begin 38.5ms。每次后台 base 与同 fixture 的同步 HDR renderer 完全相等。该入口不再等待 HDR；这些样本不包含 live compositor/IPC，也不是旧 helper 的现场 after 对比。

另一个独立 benchmark 测量被删除的 `sdrToHdr` 全图 pass：457.1–493.5ms，中位数 479.6ms。它说明该 pass 在本 fixture 上是主要成本，不能把该值直接当作旧现场试验的子耗时。新 HDR renderer 10 次（同步及后台）细分中位数：background clip 10.0ms，native real-background compose 19.7ms，foreground compose 5.0ms；原 background conversion scope 仅做 FP16 分配，约 0.16ms。此 fixture 不含 cursor，所以 cursor 0ms 不能推广到真实带光标捕获。

原始 CSV、QtTest 输出及完整细分日志保存在 [hdr-phase2b](performance/hdr-phase2b/)。

## Compositor 剩余成本

`captureWindowArtifactFromRequestFile()` 同步执行 `renderWindowArtifact()`，随后 real-background capture/blur 并写入立即返回的 artifact 响应。现有 PBO drain 路径属于 streaming；不能直接把尚未完成的 readback 放入静态响应。hover 缓存无法保证点击时的窗口帧与背景新鲜。本阶段未引入预抓取或异步 artifact 协议，点击时捕获仍保留约 230ms 的现场 compositor 成本及 UI hydration/SDR 合成。因此没有宣称点击已达到零等待；新的 `hydrate_dispatch`/`hydrate_load` 点能将它与 HDR 合成分开测量。

## 验证与现场操作

Release + BUILD_TESTING，HEIC OFF：完整构建通过；最后一轮 CTest 注册 48 项，47 项通过，1 项 scroll-live 跳过（48.02s）。新增测试涵盖被阻塞的 HDR worker 下编辑器可用、SDR 不计算 HDR、冻结快照 copy-on-write、过期任务丢弃、HDR 导出等待、关闭不 join，以及 real-background 中高光/负通道/透明和半透明前景的已知浮点结果。HEIC OFF 构建不能证明 HEIC 编码；完整编码由用户的 full build 验证。

现场脚本使用新目录 `~/.cache/hyprcapture-hdr-phase2b-20261013-final/`：`activate-before.sh`（与 Phase 1 副本 SHA 一致的原同步 helper）、`activate-after.sh`（2A + 2B）、`activate-cpu.sh`（关闭 monitor FP16，隔离 2B）、`restore.sh`。只切换 helper 与独立 timing 路径，不 unload/load 插件。保存原 helper/timing/timing_file，激活前验证二进制 SHA 与注册 handle。若在线插件被用户更换，需在新目录重新 prepare。

关闭 overlay 后运行 `activate-before.sh` 或 `activate-after.sh`，随后在 repo 根目录运行：

```sh
python3 tools/hdr-latency/editor-live.py --out ~/.cache/hyprcapture-hdr-phase2b-20261013-final --record before-window-now-cold --seconds 30
```

录制期间执行相同 flow：`open_editor()` → 点击同一 kitty（2386×1656）→ 立即或等 1s → 一格滚轮 → 取消。替换 label 为 `after-window-now-cold` 等；交替 before/after，冷/热各 5 次，保持 eDP-1 HDR 2880×1800 scale2、内容与输出格式一致，停止编译。区域模式作为控制。关闭 overlay 后运行 `restore.sh`。

`editor-summary.json` 保存 request→ready、首次 Paint、HDR base ready、wheel→thumbnail Paint、细分耗时、helper 哈希和注册 handle 检查。旧 helper 没有新的 canvas Paint 点，只能单独比较旧 `begin_edit` 完成；不可编造同端点的显示 presentation。现场 after、面板色彩和首帧验收尚待上述用户操作。
