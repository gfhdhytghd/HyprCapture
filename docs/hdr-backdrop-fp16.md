# Phase 2A：FP16 HDR backdrop

实现已完成，未提交。此阶段未切换在线插件，也未修改在线 helper 配置；Phase 1 的独立测量目录未改动。

## 实现

- 新增 capability-gated ABGR16161616F LINEAR dmabuf 路径。GPU 将冻结的 scRGB 截图转换为不透明 linear BT.2020，1.0 = 80 nit；使用 parametric ext_linear、BT.2020 和与原 CPU PQ 路径一致的参考白/峰值包络。该单位依据本机 Hyprland 0.56.2 的 EXT_LINEAR 实现验证。
- EGL/GBM 在 Qt/捕获启动期间异步预初始化。GPU fence 完成后才能发布结果；输出 BO 池最多 4 个、256 MiB。Wayland release 后才允许回收；关闭期间的未完成任务不访问窗口，关闭不等待 GPU。
- 多显示器共享 32ms 上传启动预算，首绘 native 准备最多再占用共享 4ms 预算。已完成的上传不会仅因 UI 构造超过 32ms 而丢弃。同步 HDR 子表面先提交，再由父表面的首个可见 buffer commit 一起生效。
- 能力缺失、上传失败、超时或 dmabuf 导入失败时，保留冻结 SDR 首帧并继续原 XRGB2101010/PQ worker。只有子表面实际准备好才跳过父表面的 SDR 绘制。
- 保留参考白、HDR 高光、负 scRGB/宽色域输入、premultiplied alpha 的原处理；checkerboard 奇偶与现有 CPU 路径一致。沿用窗口几何/viewport 的缩放、裁剪和 transform 处理。

## 已完成验证

Release + BUILD_TESTING，`HYPRCAPTURE_GAINMAP_HEIC=OFF`：完整构建通过；CTest 注册 48 项，47 项通过，1 项 scroll-live 按原条件跳过。新增测试覆盖能力缺失、截止时间回退、关闭期间任务，以及真实 GPU 导出的 FP16 dmabuf。

GPU 导出后经 EGL 再导入读回，与真实 `hdrToPq10` 比较：80/203/252 nit 参考白下最大 PQ 编码差分别为 0/0/1，峰值差为 0；覆盖 straight/premultiplied alpha、透明背景、灰阶、亮白、高光、负通道、宽色域和行方向图案。此为像素和元数据依据，不能代替本机面板/首帧验收。

2880×1800 含透明及彩色内容的同一冻结 fixture，顺序执行 GPU 上传完成与 CPU PQ 转换：

| 次数 | FP16 完成 ms | CPU PQ ms |
|---|---:|---:|
| 0 | 215.510 | 373.214 |
| 1 | 139.098 | 384.430 |
| 2 | 10.640 | 346.165 |
| 3 | 9.696 | 351.980 |
| 4 | 10.850 | 354.923 |

第 2–4 次稳定预热样本中位数为 FP16 10.640ms / CPU 351.980ms。前两次包含驱动初始化/惰性成本，会错过首帧预算并回退。不能据此声称冷启动必定首帧 HDR，或在线启动无回退/无回归；实际 input→paint、双层 map 和首个 HDR presentation 需现场记录。该 fixture 不同于 Phase 1，不用于跨阶段绝对耗时比较。

原始证据在 `docs/performance/hdr-phase2a/`；构建目录 `/tmp/hyprcapture-hdr-phase2a`。

## 现场切换准备

已准备 `~/.cache/hyprcapture-hdr-phase2a-20261013-final/` 的 SHA 校验副本及脚本：`activate-before.sh`（原 helper）、`activate-cpu.sh`（新 helper 禁用 FP16）、`activate-after.sh`（启用 FP16）、`restore.sh`（还原准备时的 helper）。脚本仅切换 helper，不替换 compositor 插件或修改插件计时配置；此阶段尚未执行。`state.json` 保存原设置与副本哈希。

测量需要区分原 helper 的启动基线与新 helper 的 instrumented PQ baseline。`hdr.presented` 使用 clock=1 的 CLOCK_MONOTONIC presentation timestamp，不能以日志投递时间代替；map/paint 也不能代替屏幕呈现。WAYLAND_DEBUG 的父子首个 buffer commit 检查单独进行，不混入性能样本。Phase 2B 将按其独立授权继续现场验证。
