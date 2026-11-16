# Debian 13 ARM64 板级适配

Debian 从正常发行版软件包安装。此目录保留的是移植时使用的下游配置及改动。

## 最小系统

在目标 ext4 中准备 Debian 13 ARM64，安装 `systemd-sysv`，确保 `/sbin/init` 指向 Debian 的 systemd。用下面两个精确的文件内容标记目标根目录，末尾均有换行。

| 路径 | 单行内容 |
|---|---|
| `/etc/t5-rootfs-id` | `T5_DEBIAN_EMMC_ROOT_V1` |
| `/etc/t5-release.json` | `{"schema":1,"platform":"huawei-agassi2","architecture":"arm64","root":"debian"}` |

典型 `fstab` 根条目为 `/dev/root / ext4 defaults,noatime 0 0`。只标记目标根分区。ramdisk 发现多个候选时会停住。用安装工具在目标系统内创建自己的账号及密码。

## 桌面和硬件配置

- **无线**：安装 Debian NetworkManager 和 BlueZ。按 `wireless/` 里的 unit 将 `t5-wifi-init` 放到 `/usr/local/sbin/`、`t5-hi1102` 放到 `/usr/local/libexec/`，启用对应 service。
- **音频**：将 `audio/T5HiFi.conf` 和 `audio/HiFi.conf` 装到 `/usr/share/alsa/ucm2/conf.d/T5HiFi/`。将 `ucm-microphone.conf` 附加到 `HiFi.conf`，并将两个 WirePlumber 片段装到 `/etc/wireplumber/wireplumber.conf.d/`。需要 ALSA UCM、PipeWire 和 WirePlumber。
- **电源**：`power/` 含 GNOME 快速设置扩展、schema、电源键及显示恢复服务。目录中的 `install.sh` 仅针对实际测试的 GNOME Shell 48.7-0+deb13u2。在目标 Debian 内作为 root 运行，明确传入实际使用的 GNOME 账号。
- **GNOME Shell**：`gnome-shell-48.7/` 是对 48.7 的拖放、屏幕键盘和窗口预览补丁。文件名对应 `gresource` 中的 `ui/dnd.js`、`ui/keyboard.js` 和 `ui/windowPreview.js`。可将补丁应用到匹配版本的 GNOME Shell 源码后重新构建，升级 GNOME 时需重新检查。
- **电池**：`upower/` 保存针对 UPower 1.90.9 的下游 C 源码、udev 规则与服务片段。用其中的 C 文件替换同名 `src/linux/` 文件并用 Meson/Ninja 构建 `src/upowerd`；指定服务片段使用 `/usr/local/libexec/upowerd-t5`。
- **GPU**：使用 Debian Mesa Panfrost。需要在桌面启动前允许内核的 Panfrost probe 并绑定 GPU，见 `t5-hardware` 和配套 unit。
- **启动计时**：仓库根目录的 `t5-boot-trace.py` 和 service 可按 unit 中的路径安装，记录保存在 `/var/log/t5-boot/`。

这些文件保留了板级配置以及桌面改动源码。系统包及各自的构建依赖由 Debian 提供。
