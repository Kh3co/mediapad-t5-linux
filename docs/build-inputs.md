# 本地构建输入

构建脚本从 `T5_INPUTS` 读取以下文件，默认目录为 `local-inputs/`。

| 相对路径 | 来源 |
|---|---|
| `kernel-android-original.img` | 与测试版一致的 AGS2-L03 原厂 kernel 分区，24 MiB。单启动使用它的镜像头，双启动也使用里面的 Android 内核。脚本校验 SHA-256 `9c30f4cedf72ff5bc1ac6e3d4e2c6d6f00a78123d6edaf4b5e929640017a65ec` |
| `busybox.aarch64` | 静态 ARM64 BusyBox，启用常规挂载、block、shell、文本和 coreutils applets。用发行版的 ARM64 busybox-static 或自行构建 |
| `cr4_asic.bin` | 从匹配的原厂 vendor 固件取得，使用与下方同版本的 Wi-Fi 固件 |
| `wifi76/cr4_asic.bin`、`wifi76/CPU_RAM_SCHED.bin`、`wifi76/CPU_RAM_WBS.bin` | 原厂 `/vendor/firmware/` |
| `wifi76/bfgx_and_wifi_cfg`、`wifi76/bfgx_cfg`、`wifi76/wifi_cfg` | 原厂 `/vendor/firmware/` |
| `wifi76/cfg_ags2_l03_hisi.ini` | 原厂 `/vendor/etc/` |
| `wifi76/nv193.bin`、`wifi76/nv340.bin` | 对应设备的 Hi1102 NV 记录 193 和 340 原始数据 |
| `ags2l03-hifi-nv.bin` | 对应设备在原厂音频工作时读取的 HiFi DSP NV 参数，测试长度 309608 字节 |

如果还没有设备专用 NV 数据，可以将已取得的其余输入留在本地，按 `ramdisk/initramfs.list` 维护自己的构建配置。完整构建预设会检查所有输入都存在。随机用别的机型 NV 会影响对应功能。

`T5_EMMC_CID` 对应设备上的 `/sys/class/block/mmcblk0/device/cid`。 `T5_DTS_SHA256` 使用已验证匹配的 Wi-Fi-only DTS 分区全分区 SHA-256。该检查用于内存回收前确认启动环境，确认设备配置后再设置，不能用它把任意固件视为兼容。

这套打包流程读本地文件并生成镜像。eMMC 重分区、设备固件和 Debian 安装是独立步骤。
