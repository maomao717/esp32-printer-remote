# ESP32-Printer-Remote

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)](https://www.espressif.com/)
[![Arduino](https://img.shields.io/badge/Arduino-IDE-green)](https://www.arduino.cc/)

**让旧打印机变身网络打印机 + 让手机/电脑变身无线键盘 —— 一个ESP32-S3全搞定**


## 📌 简介

ESP32-Printer-Remote 是一个基于 **ESP32-S3** 的双模式 USB 桥接器固件。它充分利用了 S3 芯片的原生 USB-OTG 功能，实现两种实用模式：

| 模式 | 功能 | 应用场景 |
|------|------|----------|
| 🖨️ **打印模式** | USB打印机 → 网络打印机 | 办公室/家庭共享老旧USB打印机 |
| 🎮 **远控模式** | 模拟键盘（有线/蓝牙） | PPT遥控、电脑远程控制、智能家居中控 |

### 特性一览

- ✅ **双模式切换**：网页后台一键切换，无需重新烧录
- ✅ **多WiFi支持**：可保存最多5个WiFi，自动连接最强信号
- ✅ **网络自适应**：支持客户端模式（连路由器）和热点模式（AP）
- ✅ **完整Web后台**：状态监控、WiFi管理、虚拟键盘、系统设置
- ✅ **WebSocket实时通信**：按键响应迅速，延迟低
- ✅ **有线+蓝牙双远控**：USB有线（零延迟）或蓝牙无线（自由），自动切换


## 🛠️ 硬件要求

| 项目 | 要求 |
|------|------|
| 开发板 | **ESP32-S3**（必需，其他型号无原生USB-OTG） |
| USB接口 | 需要原生USB口（USB-C或Micro-USB均可） |
| 打印机 | 支持标准USB打印协议（HP、Epson、Canon、Brother等） |
| 供电 | USB供电即可，无需额外电源 |

⚠️ **注意**：ESP32（经典版）、ESP32-C3、ESP32-S2 均不适用本固件，必须使用 **ESP32-S3**。


## 🚀 快速开始

### 1. 环境准备

- 安装 [Arduino IDE](https://www.arduino.cc/en/software)（推荐2.x版本）
- 安装ESP32开发板支持：工具 → 开发板管理器 → 搜索 `esp32` 安装
- 安装依赖库（库管理器搜索安装）：
  - `WebSockets` by Markus Sattler
  - `ArduinoJson` by Benoit Blanchon
  - `BleKeyboard` by T-vK
  - `Adafruit TinyUSB` by Adafruit

### 2. 开发板设置（重要！）

在 `工具` 菜单中设置以下选项：

| 设置项 | 值 |
|--------|-----|
| Board | **ESP32S3 Dev Module** |
| USB Mode | **USB-OTG (TinyUSB)** |
| USB CDC On Boot | **Disabled** |
| Partition Scheme | Default 4MB with spiffs |

### 3. 烧录固件

```bash
git clone https://github.com/maomao717/ESP32-Printer-Remote.git
```
- 用Arduino IDE打开 `ESP32_Printer_Remote.ino`
- 选择正确的端口，点击"上传"

### 4. 首次配置

1. 烧录完成后，ESP32-S3 会创建热点：**ESP32-Bridge**，密码：`12345678`
2. 手机/电脑连接该热点
3. 浏览器访问 **`http://192.168.4.1`**
4. 在"WiFi管理"中添加你家的WiFi信息
5. 设备自动切换至内网模式，记下新的IP地址


## 📱 使用指南

### 打印模式

1. 将USB打印机连接到ESP32-S3的**原生USB口**
2. 确保ESP32已连接到网络
3. 在Windows中添加打印机：
   - 控制面板 → 设备和打印机 → 添加打印机
   - 选择"通过手动设置添加本地打印机或网络打印机"
   - 创建新端口 → Standard TCP/IP Port
   - 输入ESP32的IP地址，端口号 `9100`
   - 选择你的打印机驱动完成安装

### 远控模式

1. 切换至远控模式（网页后台点击"远控模式"）
2. 控制方式选择：
   - **有线模式**：用USB线将ESP32-S3连接电脑，电脑自动识别为键盘
   - **蓝牙模式**：电脑蓝牙搜索 `ESP32 Remote` 并配对
3. 访问网页后台，使用虚拟键盘控制电脑：
   - 点击按键 → 电脑收到对应按键
   - 输入文本 → 电脑自动打字
   - 组合键：先点亮 Ctrl/Alt/Shift/Win，再点击目标键
   - 媒体控制：音量、播放/暂停等


## 🌐 网页后台功能

| 页面 | 功能 |
|------|------|
| 状态仪表盘 | 当前模式、IP地址、WiFi信号、内存使用 |
| 打印模式 | RAW端口信息、Windows添加教程 |
| 远控模式 | 虚拟键盘、文本发送、修饰键、媒体控制 |
| WiFi管理 | 扫描WiFi、查看已保存网络、手动添加/删除 |
| 系统设置 | 重启设备 |


## 📡 API接口

适用于二次开发，可通过HTTP请求控制设备：

```bash
# 获取状态
GET /api/status

# 切换模式
GET /api/mode?mode=printer   # 打印模式
GET /api/mode?mode=remote    # 远控模式

# 扫描WiFi
GET /api/scan

# 获取WiFi列表
GET /api/networks

# 添加WiFi
POST /api/wifi/add
Content-Type: application/json
{"ssid":"你的WiFi", "password":"密码"}

# 清空WiFi
GET /api/wifi/clear

# 重启设备
GET /api/reboot
```


## 📁 项目结构

```
ESP32-Printer-Remote/
├── ESP32_Printer_Remote.ino   # 主程序
├── README.md                   # 项目说明
├── LICENSE                     # MIT许可证
└── docs/
    ├── wiring.md               # 硬件接线说明
    └── api.md                  # API详细文档
```


## 📄 许可证

MIT License


## 🤝 贡献

欢迎提交Issue和Pull Request！


## 📧 联系方式

- 作者：[你的名字]
- GitHub：[你的GitHub用户名]
- 项目地址：https://github.com/你的用户名/ESP32-Printer-Remote
```

---

## 建议的 Git 忽略文件 (.gitignore)

```gitignore
# Arduino
*.ino.precompiled
*.ino.tmp
*.ino.with_bootloader.bin
*.ino.bin
*.ino.dSYM/
*.ino.elf
*.ino.eep
*.ino.hex
*.ino.map

# PlatformIO
.pio/
.pioenvs/
.piolibdeps/

# IDE
.vscode/
.idea/
*.swp
*.swo

# Build outputs
/build/
/output/

# Personal config
/secrets.h
/preferences.dat
```

---

