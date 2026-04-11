# ESP32-S3 CANable SLCAN

ESP32-S3 + SN65HVD230 的 CANable SLCAN 固件，基于 CANable 2.5 协议规范。

## 特性

- **USB VID/PID**: 0x16D0:0x117E（CANable 官方 Vendor ID）
- **USB CDC**: 即插即用，无需串口驱动
- **DTR 兼容**: 扫描软件不拉 DTR 时仍能正常收发数据
- **SLCAN 协议**: 完整实现 CANable 2.5 Classic CAN 命令集
- **BusOff 自动恢复**: 总线故障后自动尝试恢复

## 硬件接线

```
ESP32-S3          SN65HVD230          OBD-II / CAN Bus
─────────         ──────────          ────────────────
GPIO 4  ────────  TXD
GPIO 5  ────────  RXD
3.3V    ────────  VCC
GND     ────────  GND
                     CANH    ────────  CAN_H (Pin 6)
                     CANL    ────────  CAN_L (Pin 14)
```

> ⚠️ 如果 CAN 总线两端都没有终端电阻，在 CANH 和 CANL 之间加一个 120Ω 电阻。

### ESP32-S3 注意事项

- 使用 **USB-OTG** 接口（不是 USB-Serial/JTAG）
- 部分 ESP32-S3 开发板需要手动跳线连接 USB
- 烧录时需通过 USB-Serial/JTAG 口（BOOT + RST 进入下载模式）

## 编译 & 烧录

**仅支持 PlatformIO**（本项目使用自定义板型定义，无 Arduino IDE .ino 文件）。

### 环境要求

- [PlatformIO CLI](https://platformio.org/install/cli) 或 [VS Code + PlatformIO 插件](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
- ESP32-S3 开发板 + SN65HVD230 CAN 收发器模块

### 编译上传

```bash
# 克隆仓库
git clone https://github.com/YOUR_USERNAME/esp32-s3-canable-slcan.git
cd esp32-s3-canable-slcan

# 编译
pio run

# 烧录（通过 USB-Serial/JTAG）
pio run --target upload --upload-port COM6
```

> 💡 如果 ESP32 只有一个 USB 口且 TinyUSB 已占用，用 1200 波特率触发重启进入下载模式：
> ```python
> import serial, time
> s = serial.Serial('COM7', 1200)  # COM7 = TinyUSB CDC 口
> time.sleep(0.5)
> s.close()
> # COM6 会在几秒内重新出现
> ```

## SLCAN 命令

| 命令 | 说明 | 状态 |
|------|------|:----:|
| `O` / `ON` | 打开 CAN（普通模式） | ✅ |
| `OS` | 打开 CAN（只听模式） | ✅ |
| `C` | 关闭 CAN | ✅ |
| `S0`-`S8` | 设置波特率 (10k-1M) | ✅ |
| `S9` | 设置 83k 波特率 | ✅ |
| `t` | 发送标准帧 | ✅ |
| `T` | 发送扩展帧 | ✅ |
| `r` / `R` | 发送 RTR 帧 | ✅ |
| `Z0`/`Z1` | 时间戳开关 | ✅ |
| `V` | 版本信息 | ✅ |
| `N` | 序列号 | ✅ |
| `ME`/`Me` | 错误报告开关 | ✅ |
| `MF`/`Mf` | 命令反馈开关 | ✅ |
| `MD`/`Md` | 调试消息开关 | ✅ |
| `MM`/`Mm` | Tx 回显开关 | ✅ |
| `MA`/`Ma` | 自动重传开关 | ✅ |
| `A0`/`A1` | 自动重传（传统） | ✅ |
| `F` | 设置/读取滤波器 | ✅ |
| `f` | 清除滤波器 | ✅ |
| `L` | 总线负载报告 | ✅¹ |
| `d/D/b/B` | CAN FD 帧 | ❌² |
| `s` | 自定义波特率 | ❌³ |
| `*DFU` | 固件升级 | ❌⁴ |

> ¹ 总线负载报告始终返回 0（ESP32 TWAI 不提供此指标）
> ² ESP32-S3 TWAI 仅支持 Classic CAN 2.0，不支持 CAN FD
> ³ TWAI 不支持 SLCAN 格式的自定义位时序
> ⁴ ESP32 使用 esptool 烧录，不走 DFU 模式

## 项目结构

```
├── src/
│   ├── main.cpp          # SLCAN 协议实现 + CAN 驱动
│   └── dtr_fix.c         # TinyUSB DTR 兼容性修复
├── boards/
│   └── esp32s3_slcan.json # PlatformIO 自定义板型定义
├── fix_usb_strings.py    # 构建后处理：注入 USB 字符串
├── platformio.ini        # PlatformIO 配置
├── .gitignore
└── LICENSE
```

## 技术细节

### USB VID/PID

使用 CANable 官方 Vendor ID（0x16D0:0x117E），在 `boards/esp32s3_slcan.json` 和 `platformio.ini` 中配置。

### TinyUSB DTR 修复 (`dtr_fix.c`)

**问题**: 部分 OBD2 扫描软件打开串口时不拉高 DTR 信号。TinyUSB 内部的 `tud_cdc_n_connected()` 检查 DTR 状态，DTR 为低时会静默丢弃所有 TX 数据，导致 SLCAN 命令的回复被吞掉。

**方案**: 通过链接器 `--wrap=tud_cdc_n_connected` 重定向该函数，始终返回 `true`：

```c
// dtr_fix.c
bool __wrap_tud_cdc_n_connected(uint8_t itf) {
    (void)itf;
    return true;
}
```

对应 `platformio.ini` 中的 `-Wl,--wrap=tud_cdc_n_connected`。

### 框架补丁

本固件依赖 Arduino-ESP32 框架的运行时行为：`USBCDC::begin()` 需要调用 `USB.begin()` 初始化 TinyUSB 设备。框架版本 `3.20017.x` 中已包含此修复，如使用旧版框架可能需要手动添加。

## 与 CANable 2.5 的兼容性

CANable 2.5 固件运行在 STM32G4 的 FDCAN 外设上，支持 CAN FD（最高 8 Mbps）。ESP32-S3 的 TWAI 仅支持 Classic CAN 2.0（最高 1 Mbps）：

- ✅ Classic CAN 帧完全兼容
- ❌ CAN FD 帧不支持（`d/D/b/B` 命令返回错误码 `6`）

## License

MIT
