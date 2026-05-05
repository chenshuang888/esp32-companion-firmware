# ESP32 桌面伴侣固件

> **🌐 桌面伴侣项目（共 4 仓）**
> [开发主仓 / 设备固件源](https://github.com/chenshuang888/esp32-desktop-companion)
> · **▸ 本仓：设备固件发布版**
> · [Windows 桌面端](https://github.com/chenshuang888/esp32-companion-app)
> · [应用市场](https://github.com/chenshuang888/esp32-marketplace)（[在线 demo](https://marketplace.chenshuang.fun)）

ESP32-S3 触摸屏桌面伴侣的设备端固件。配套 [桌面端 App](https://github.com/chenshuang888/esp32-companion-app) 通过 BLE 联动，并支持从 [App 市场](https://github.com/chenshuang888/esp32-marketplace) 一键安装动态小程序。

> 默认 BLE 设备名：`ESP32-S3-DEMO`

---

## 硬件要求

- **MCU**：ESP32-S3 N16R8（16MB Flash QIO / 8MB PSRAM Octal）
- **屏幕**：ST7789 240×320 SPI LCD
- **触摸**：FT5x06 电容触摸屏（I²C）
- **背光**：LEDC PWM 控制

### 引脚连接

| 外设 | GPIO | 外设 | GPIO |
|---|---|---|---|
| LCD_SCK | 12 | TOUCH_SCL | 18 |
| LCD_MOSI | 11 | TOUCH_SDA | 17 |
| LCD_CS | 10 | TOUCH_RST | 15 |
| LCD_DC | 9 | TOUCH_INT | 16 |
| LCD_RST | 8 | LCD_BL | 14 |

引脚与显示参数集中在 `drivers/board_config.h`，板子不同改这一份即可。

> ⚠️ N16R8 的 Flash 是 **QIO** 而非 Octal，`CONFIG_ESPTOOLPY_OCT_FLASH` 必须保持 OFF，否则会 boot 循环。

---

## 烧录使用（普通用户）

### 1. 安装 ESP-IDF

需要 **ESP-IDF v5.4.3 或以上**。Windows 推荐使用 [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/) 一键安装。

### 2. 克隆并编译

```bash
git clone https://github.com/chenshuang888/esp32-companion-firmware.git
cd esp32-companion-firmware

# 激活 ESP-IDF 环境（Windows）
%USERPROFILE%\esp\v5.4.3\esp-idf\export.bat

# Linux/macOS
. $HOME/esp/esp-idf/export.sh

# 设置目标芯片并编译
idf.py set-target esp32s3
idf.py build
```

### 3. 烧录

```bash
# 第一次烧录必须先擦除 Flash（LittleFS 分区 magic 校验）
idf.py -p COM3 erase-flash

# 烧录 + 监视串口
idf.py -p COM3 flash monitor
```

> Windows 端口形如 `COM3`，Linux 形如 `/dev/ttyUSB0`，macOS 形如 `/dev/cu.usbserial-*`。

### 4. 配对 PC 端

烧录成功后设备 BLE 广播 `ESP32-S3-DEMO`。下载 [桌面端 App](https://github.com/chenshuang888/esp32-companion-app/releases) 双击运行即可自动连接。

---

## 分区表

```
nvs       0x9000    24KB        BLE 配对、设置、通知快照
phy_init  0xf000    4KB         RF 校准
factory   0x10000   6MB         主应用（含中文 TTF + LVGL）
storage   auto      ~9.9MB      LittleFS：动态 App 脚本仓
```

切换分区表或第一次刷机必须 `idf.py erase-flash`，否则 LittleFS 挂载失败。

---

## 项目结构

```
.
├── main/                启动入口
├── app/                 UI 层（LVGL 页面 + 字体 + 图标）
│   └── apps/            内建页面：锁屏 / 启动器 / 时钟 / 天气 / 通知 / 音乐 / 系统 / 设置 / 动态 App 宿主
├── framework/           顶层 + 子页面路由器
├── drivers/             驱动：LCD / 触摸 / LVGL 移植 / BLE
├── services/            BLE 服务 + 数据管理（CTS/天气/通知/媒体/系统/动态 App 桥）
├── storage/             NVS + LittleFS 抽象
├── dynamic_app/         JS 动态 App 框架（基于 MicroQuickJS）
├── components/          本地三方组件（已 vendored，含必要补丁）
├── partitions.csv       分区表
└── sdkconfig.defaults   工程默认配置
```

---

## 常见问题

- **build 报错 `TEMPERATURE_SENSOR_CLK_SRC_DEFAULT undeclared`**：忘了 `idf.py set-target esp32s3`，target 默认为 esp32。删 `sdkconfig` 重新 set-target 即可。
- **设备启动后白屏**：通常是 Flash 模式不对（必须 QIO）或第一次刷机没 `erase-flash`。
- **PC 连不上**：确认蓝牙开启、设备在通电、BLE 名一致；多台同名设备可在 PC 端 `config.json` 锁定 MAC。
- **想改 BLE 名**：搜索 `ESP32-S3-DEMO` 替换。

---

## 第三方组件

| 组件 | 用途 |
|---|---|
| LVGL 9.x | GUI |
| esp_lcd_touch_ft5x06 | 触摸驱动 |
| joltwallet/littlefs | 动态 App 文件系统 |
| makgordon/esp-mquickjs | 动态 App JS 引擎（**已 vendored 到 `components/`，含本地补丁**） |
| NimBLE（IDF 内置） | BLE 协议栈 |

---

## 许可证

[MIT](LICENSE) © 2026 ChenShuang
