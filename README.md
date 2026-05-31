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
git clone https://github.com/你的用户名/ESP32-Printer-Remote.git
