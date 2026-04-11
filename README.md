# ESP32-S3 CANable SLCAN

[English](#english) / [中文](#中文)

---

## English

ESP32-S3 + SN65HVD230 CANable SLCAN firmware based on the CANable 2.5 protocol specification.

### Features

- **USB VID/PID**: 0x16D0:0x117E (official CANable Vendor ID)
- **USB CDC**: Plug and play, no driver required
- **DTR compatibility**: Works correctly even when scanner software does not assert DTR
- **SLCAN protocol**: Full CANable 2.5 Classic CAN command set
- **BusOff auto-recovery**: Automatically recovers from bus errors

### Wiring

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

> ⚠️ If neither end of the CAN bus has a termination resistor, add a 120Ω resistor between CANH and CANL.

#### ESP32-S3 Notes

- Use the **USB-OTG** port (not USB-Serial/JTAG)
- Some ESP32-S3 dev boards require manual jumper wires for USB
- Flash via the USB-Serial/JTAG port (hold BOOT, press RST to enter download mode)

### Build & Flash

**PlatformIO only** (this project uses a custom board definition; there is no Arduino IDE .ino file).

#### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli) or [VS Code + PlatformIO extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
- ESP32-S3 dev board + SN65HVD230 CAN transceiver module

#### Compile & Upload

```bash
git clone https://github.com/l2015/esp32s3-slcan-canable.git
cd esp32s3-slcan-canable

# Build
pio run

# Flash (via USB-Serial/JTAG)
pio run --target upload --upload-port COM6
```

> 💡 If the ESP32 has only one USB port and TinyUSB is occupying it, trigger a reset via 1200 baud:
> ```python
> import serial, time
> s = serial.Serial('COM7', 1200)  # COM7 = TinyUSB CDC port
> time.sleep(0.5)
> s.close()
> # COM6 will reappear in a few seconds
> ```

### SLCAN Commands

| Command | Description | Status |
|---------|-------------|:------:|
| `O` / `ON` | Open CAN (normal mode) | ✅ |
| `OS` | Open CAN (listen-only mode) | ✅ |
| `C` | Close CAN | ✅ |
| `S0`-`S8` | Set baud rate (10k-1M) | ✅ |
| `S9` | Set 83k baud rate | ✅ |
| `t` | Transmit standard frame | ✅ |
| `T` | Transmit extended frame | ✅ |
| `r` / `R` | Transmit RTR frame | ✅ |
| `Z0`/`Z1` | Timestamp toggle | ✅ |
| `V` | Version info | ✅ |
| `N` | Serial number | ✅ |
| `ME`/`Me` | Error reporting toggle | ✅ |
| `MF`/`Mf` | Command feedback toggle | ✅ |
| `MD`/`Md` | Debug message toggle | ✅ |
| `MM`/`Mm` | Tx echo toggle | ✅ |
| `MA`/`Ma` | Auto-retransmit toggle | ✅ |
| `A0`/`A1` | Auto-retransmit (legacy) | ✅ |
| `F` | Set/read filter | ✅ |
| `f` | Clear filter | ✅ |
| `L` | Bus load report | ✅¹ |
| `d/D/b/B` | CAN FD frames | ❌² |
| `s` | Custom baud rate | ❌³ |
| `*DFU` | Firmware update | ❌⁴ |

> ¹ Bus load always returns 0 (ESP32 TWAI does not expose this metric)
> ² ESP32-S3 TWAI supports Classic CAN 2.0 only, not CAN FD
> ³ TWAI does not support SLCAN-format custom bit timing
> ⁴ ESP32 uses esptool for flashing, not DFU mode

### Project Structure

```
├── src/
│   ├── main.cpp          # SLCAN protocol implementation + CAN driver
│   └── dtr_fix.c         # TinyUSB DTR compatibility fix
├── boards/
│   └── esp32s3_slcan.json # PlatformIO custom board definition
├── fix_usb_strings.py    # Post-build: inject USB descriptor strings
├── platformio.ini        # PlatformIO configuration
├── .gitignore
└── LICENSE
```

### Technical Details

#### USB VID/PID

Uses the official CANable Vendor ID (0x16D0:0x117E), configured in `boards/esp32s3_slcan.json` and `platformio.ini`.

#### TinyUSB DTR Fix (`dtr_fix.c`)

**Problem**: Some OBD2 scanner software does not assert DTR when opening the serial port. TinyUSB internally checks DTR via `tud_cdc_n_connected()`, and silently discards all TX data when DTR is low, causing SLCAN command responses to be swallowed.

**Solution**: Use the linker `--wrap=tud_cdc_n_connected` to redirect the function to always return `true`:

```c
// dtr_fix.c
bool __wrap_tud_cdc_n_connected(uint8_t itf) {
    (void)itf;
    return true;
}
```

Corresponds to `-Wl,--wrap=tud_cdc_n_connected` in `platformio.ini`.

### CANable 2.5 Compatibility

The CANable 2.5 firmware runs on an STM32G4 FDCAN peripheral and supports CAN FD (up to 8 Mbps). The ESP32-S3 TWAI peripheral supports **Classic CAN 2.0 only** (up to 1 Mbps):

- ✅ Classic CAN frames fully compatible
- ❌ CAN FD frames not supported (`d/D/b/B` commands return error code `6`)

---

## 中文

ESP32-S3 + SN65HVD230 的 CANable SLCAN 固件，基于 CANable 2.5 协议规范。

### 特性

- **USB VID/PID**: 0x16D0:0x117E（CANable 官方 Vendor ID）
- **USB CDC**: 即插即用，无需串口驱动
- **DTR 兼容**: 扫描软件不拉 DTR 时仍能正常收发数据
- **SLCAN 协议**: 完整实现 CANable 2.5 Classic CAN 命令集
- **BusOff 自动恢复**: 总线故障后自动尝试恢复

### 硬件接线

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

#### ESP32-S3 注意事项

- 使用 **USB-OTG** 接口（不是 USB-Serial/JTAG）
- 部分 ESP32-S3 开发板需要手动跳线连接 USB
- 烧录时需通过 USB-Serial/JTAG 口（BOOT + RST 进入下载模式）

### 编译 & 烧录

**仅支持 PlatformIO**（本项目使用自定义板型定义）。

#### 环境要求

- [PlatformIO CLI](https://platformio.org/install/cli) 或 [VS Code + PlatformIO 插件](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
- ESP32-S3 开发板 + SN65HVD230 CAN 收发器模块

#### 编译上传

```bash
git clone https://github.com/l2015/esp32s3-slcan-canable.git
cd esp32s3-slcan-canable

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

### SLCAN 命令

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

### 技术细节

#### USB VID/PID

使用 CANable 官方 Vendor ID（0x16D0:0x117E），在 `boards/esp32s3_slcan.json` 和 `platformio.ini` 中配置。

#### TinyUSB DTR 修复 (`dtr_fix.c`)

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

### 与 CANable 2.5 的兼容性

CANable 2.5 固件运行在 STM32G4 的 FDCAN 外设上，支持 CAN FD（最高 8 Mbps）。ESP32-S3 的 TWAI 仅支持 Classic CAN 2.0（最高 1 Mbps）：

- ✅ Classic CAN 帧完全兼容
- ❌ CAN FD 帧不支持（`d/D/b/B` 命令返回错误码 `6`）

---

## License

MIT
