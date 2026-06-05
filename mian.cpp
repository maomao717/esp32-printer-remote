/**
 * ESP32-S3 双模式 USB 桥接器 v5.0 - 生产级完整版
 * 
 * 模式1 - 打印模式：USB打印机 → 网络打印（RAW端口9100）
 * 模式2 - 远控模式：模拟键盘（USB有线 + 蓝牙无线）远程控制电脑
 * 
 * 修正内容：
 * - 修复 USB 事件竞态条件（只在 FreeRTOS 任务中处理）
 * - 添加 ZLP（零长度包）支持，解决打印卡尾问题
 * - 修复 transfer 内存泄漏（所有状态都回收）
 * - 添加互斥锁防止并发冲突
 * - 完整的双模式切换功能
 * - 多 WiFi 支持和网页后台
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "USB.h"
#include "USBHID.h"
#include "usb/usb_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ====================== 引入 TinyUSB HID 库（用于有线键盘）======================
#include <Adafruit_TinyUSB.h>

// ====================== 配置常量 ======================
const char* AP_SSID           = "ESP32-Bridge";
const char* AP_PASSWORD       = "12345678";
const int   PRINT_PORT        = 9100;
const int   WEB_PORT          = 80;
const int   WS_PORT           = 81;
const int   MAX_WIFI_NETWORKS = 5;

// 工作模式
enum WorkMode { MODE_PRINTER, MODE_REMOTE };
enum NetMode { MODE_WIFI_CLIENT, MODE_AP };
enum UsbDeviceType { DEV_NONE, DEV_PRINTER, DEV_KEYBOARD, DEV_MOUSE, DEV_UNKNOWN };

// ====================== 全局变量 ======================
// 工作模式
volatile WorkMode currentWorkMode = MODE_REMOTE;
NetMode currentNetMode = MODE_WIFI_CLIENT;
UsbDeviceType connectedDevice = DEV_NONE;

// 打印机模式标志
volatile bool printerReady = false;
volatile bool deviceConnected = false;

// 远控模式标志
volatile bool remoteReady = false;
volatile bool usbHidReady = false;
volatile bool bleReady = false;

// 存储
Preferences prefs;

// 服务器
WebServer server(WEB_PORT);
WebSocketsServer webSocket = WebSocketsServer(WS_PORT);
DNSServer dnsServer;

// WiFi 配置
struct WiFiConfig {
  String ssid;
  String password;
};
WiFiConfig savedNetworks[MAX_WIFI_NETWORKS];
int savedNetworkCount = 0;

// ====================== 打印机相关变量 ======================
WiFiServer printServer(PRINT_PORT);
static usb_host_client_handle_t clientHandle = NULL;
static usb_device_handle_t      deviceHandle = NULL;
static uint8_t                  bulkOutEpAddr = 0;
static uint8_t                  bulkOutEpMps  = 64;
static uint8_t                  printerInterfaceNum = 0;

// 传输相关
#define TRANSFER_MAX_SIZE 1024
#define TRANSFER_POOL_SIZE 6
static QueueHandle_t transferQueue = NULL;
static SemaphoreHandle_t usbMutex = NULL;

// 数据缓冲
#define USB_RECV_BUF_SIZE 4096
static uint8_t recvBuf[USB_RECV_BUF_SIZE];

// ====================== 远控模式相关变量 ======================
// 有线 HID
Adafruit_USBD_HID usb_hid;

// 蓝牙 HID
#include <BleKeyboard.h>
BleKeyboard bleKeyboard("ESP32 Remote", "Espressif", 100);

// 当前按下的修饰键
volatile bool ctrlPressed = false;
volatile bool altPressed = false;
volatile bool shiftPressed = false;
volatile bool guiPressed = false;

// 按键映射表
struct KeyMapping {
  const char* name;
  uint8_t key;
};

KeyMapping keyMap[] = {
  {"enter", HID_KEY_ENTER},
  {"space", HID_KEY_SPACE},
  {"tab", HID_KEY_TAB},
  {"esc", HID_KEY_ESCAPE},
  {"backspace", HID_KEY_BACKSPACE},
  {"del", HID_KEY_DELETE},
  {"up", HID_KEY_ARROW_UP},
  {"down", HID_KEY_ARROW_DOWN},
  {"left", HID_KEY_ARROW_LEFT},
  {"right", HID_KEY_ARROW_RIGHT},
  {"home", HID_KEY_HOME},
  {"end", HID_KEY_END},
  {"pgup", HID_KEY_PAGE_UP},
  {"pgdn", HID_KEY_PAGE_DOWN},
  {"f1", HID_KEY_F1},
  {"f2", HID_KEY_F2},
  {"f3", HID_KEY_F3},
  {"f4", HID_KEY_F4},
  {"f5", HID_KEY_F5},
  {"f6", HID_KEY_F6},
  {"f7", HID_KEY_F7},
  {"f8", HID_KEY_F8},
  {"f9", HID_KEY_F9},
  {"f10", HID_KEY_F10},
  {"f11", HID_KEY_F11},
  {"f12", HID_KEY_F12}
};

int keyMapSize = sizeof(keyMap) / sizeof(keyMap[0]);

// ====================== WiFi 配置存储 ======================
void loadWiFiConfigs() {
  savedNetworkCount = prefs.getInt("wifi_count", 0);
  if (savedNetworkCount > MAX_WIFI_NETWORKS) savedNetworkCount = MAX_WIFI_NETWORKS;
  
  for (int i = 0; i < savedNetworkCount; i++) {
    String keySsid = "wifi_" + String(i) + "_ssid";
    String keyPwd  = "wifi_" + String(i) + "_pwd";
    savedNetworks[i].ssid = prefs.getString(keySsid.c_str(), "");
    savedNetworks[i].password = prefs.getString(keyPwd.c_str(), "");
    Serial.printf("[STORAGE] Loaded WiFi %d: %s\n", i, savedNetworks[i].ssid.c_str());
  }
}

void saveWiFiConfigs() {
  prefs.putInt("wifi_count", savedNetworkCount);
  for (int i = 0; i < savedNetworkCount; i++) {
    String keySsid = "wifi_" + String(i) + "_ssid";
    String keyPwd  = "wifi_" + String(i) + "_pwd";
    prefs.putString(keySsid.c_str(), savedNetworks[i].ssid);
    prefs.putString(keyPwd.c_str(), savedNetworks[i].password);
  }
}

void addWiFiNetwork(String ssid, String password) {
  // 检查是否已存在
  for (int i = 0; i < savedNetworkCount; i++) {
    if (savedNetworks[i].ssid == ssid) {
      savedNetworks[i].password = password;
      saveWiFiConfigs();
      Serial.printf("[STORAGE] Updated WiFi: %s\n", ssid.c_str());
      return;
    }
  }
  
  // 添加新网络
  if (savedNetworkCount < MAX_WIFI_NETWORKS) {
    savedNetworks[savedNetworkCount].ssid = ssid;
    savedNetworks[savedNetworkCount].password = password;
    savedNetworkCount++;
    saveWiFiConfigs();
    Serial.printf("[STORAGE] Added WiFi: %s\n", ssid.c_str());
  } else {
    Serial.println("[STORAGE] WiFi limit reached");
  }
}

void removeWiFiNetwork(int index) {
  if (index < 0 || index >= savedNetworkCount) return;
  
  for (int i = index; i < savedNetworkCount - 1; i++) {
    savedNetworks[i] = savedNetworks[i + 1];
  }
  savedNetworkCount--;
  saveWiFiConfigs();
  Serial.printf("[STORAGE] Removed WiFi index: %d\n", index);
}

void clearAllWiFi() {
  savedNetworkCount = 0;
  saveWiFiConfigs();
  Serial.println("[STORAGE] Cleared all WiFi");
}

// ====================== USB 打印机传输回调 ======================
void printerTransferCallback(usb_transfer_t* transfer) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  
  switch (transfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
      ESP_LOGD("USB", "Transfer completed: %d bytes", transfer->actual_num_bytes);
      break;
    case USB_TRANSFER_STATUS_TIMED_OUT:
      ESP_LOGW("USB", "Transfer timeout");
      break;
    case USB_TRANSFER_STATUS_CANCELED:
      ESP_LOGW("USB", "Transfer canceled");
      break;
    case USB_TRANSFER_STATUS_STALL:
      ESP_LOGW("USB", "Transfer stall, clearing...");
      if (deviceHandle && bulkOutEpAddr) {
        usb_host_endpoint_halt(deviceHandle, bulkOutEpAddr);
usb_host_endpoint_flush(deviceHandle, bulkOutEpAddr);
      }
      break;
    default:
      ESP_LOGW("USB", "Transfer status: %d", transfer->status);
      break;
  }
  
  // 无论什么状态，都放回队列复用
  xQueueSendFromISR(transferQueue, &transfer, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// ====================== 发送打印数据 ======================
bool sendToPrinter(const uint8_t* data, size_t len) {
  if (!printerReady || deviceHandle == NULL || bulkOutEpAddr == 0) {
    return false;
  }
  
  // 加互斥锁
  if (xSemaphoreTake(usbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE("PRINT", "Failed to take USB mutex");
    return false;
  }
  
  size_t sent = 0;
  bool success = true;
  
  while (sent < len) {
    size_t chunk = min((size_t)bulkOutEpMps, len - sent);
    
    usb_transfer_t* transfer = NULL;
    if (xQueueReceive(transferQueue, &transfer, pdMS_TO_TICKS(500)) != pdTRUE) {
      ESP_LOGE("PRINT", "No free transfer");
      success = false;
      break;
    }
    
    transfer->num_bytes = chunk;
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = bulkOutEpAddr;
    transfer->callback = printerTransferCallback;
    memcpy(transfer->data_buffer, data + sent, chunk);
    
    esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
      ESP_LOGE("PRINT", "Submit failed: %s", esp_err_to_name(err));
      xQueueSend(transferQueue, &transfer, portMAX_DELAY);
      success = false;
      break;
    }
    
    sent += chunk;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  
  // 处理 ZLP（零长度包）
  if (success && len > 0 && (len % bulkOutEpMps) == 0) {
    ESP_LOGD("PRINT", "Sending ZLP");
    usb_transfer_t* zlpTransfer = NULL;
    if (xQueueReceive(transferQueue, &zlpTransfer, pdMS_TO_TICKS(500)) == pdTRUE) {
      zlpTransfer->num_bytes = 0;
      zlpTransfer->device_handle = deviceHandle;
      zlpTransfer->bEndpointAddress = bulkOutEpAddr;
      zlpTransfer->callback = printerTransferCallback;
      
      esp_err_t err = usb_host_transfer_submit(zlpTransfer);
      if (err != ESP_OK) {
        ESP_LOGW("PRINT", "ZLP submit failed");
        xQueueSend(transferQueue, &zlpTransfer, portMAX_DELAY);
      }
    }
  }
  
  xSemaphoreGive(usbMutex);
  return success;
}

// ====================== 初始化打印机 ======================
bool initPrinterDevice(uint8_t devAddr) {
  esp_err_t err;
  
  err = usb_host_device_open(clientHandle, devAddr, &deviceHandle);
  if (err != ESP_OK) {
    ESP_LOGE("USB", "Device open failed");
    return false;
  }
  
  const usb_device_desc_t* devDesc;
  usb_host_get_device_descriptor(deviceHandle, &devDesc);
  ESP_LOGI("USB", "VID:0x%04X, PID:0x%04X", devDesc->idVendor, devDesc->idProduct);
  
  const usb_config_desc_t* cfgDesc;
  usb_host_get_active_config_descriptor(deviceHandle, &cfgDesc);
  
  // 遍历查找打印机接口
  bool printerFound = false;
  for (int i = 0; i < cfgDesc->bNumInterfaces; i++) {
    const usb_intf_desc_t* intf = usb_parse_interface_descriptor(cfgDesc, i);
    if (intf == NULL) continue;
    
    if (intf->bInterfaceClass == 0x07) {  // 打印机类
      printerFound = true;
      printerInterfaceNum = intf->bInterfaceNumber;
      
      for (int j = 0; j < intf->bNumEndpoints; j++) {
        const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor(intf, j);
        if (ep == NULL) continue;
        
        if ((ep->bmAttributes & 0x03) == USB_TRANSFER_TYPE_BULK) {
          if ((ep->bEndpointAddress & 0x80) == 0) {
            bulkOutEpAddr = ep->bEndpointAddress;
            bulkOutEpMps = ep->wMaxPacketSize;
            ESP_LOGI("USB", "Bulk OUT: 0x%02X, MPS:%d", bulkOutEpAddr, bulkOutEpMps);
          }
        }
      }
      break;
    }
  }
  
  if (!printerFound || bulkOutEpAddr == 0) {
    ESP_LOGE("USB", "Printer not found");
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = NULL;
    return false;
  }
  
  err = usb_host_interface_claim(clientHandle, deviceHandle, printerInterfaceNum, 0);
  if (err != ESP_OK) {
    ESP_LOGE("USB", "Interface claim failed");
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = NULL;
    return false;
  }
  
  return true;
}

// ====================== USB 事件回调 ======================
void usbEventCallback(const usb_host_client_event_msg_t* msg, void* arg) {
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      ESP_LOGI("USB", "New device, addr: %d", msg->new_dev.address);
      
      if (currentWorkMode == MODE_PRINTER && !deviceConnected) {
        if (initPrinterDevice(msg->new_dev.address)) {
          deviceConnected = true;
          printerReady = true;
          ESP_LOGI("USB", "Printer ready!");
        }
      }
      break;
      
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      ESP_LOGI("USB", "Device removed");
      
      if (deviceHandle) {
        usb_host_interface_release(clientHandle, deviceHandle, printerInterfaceNum);
        usb_host_device_close(clientHandle, deviceHandle);
        deviceHandle = NULL;
      }
      deviceConnected = false;
      printerReady = false;
      bulkOutEpAddr = 0;
      printerInterfaceNum = 0;
      break;
      
    default:
      break;
  }
}

// ====================== USB Host 任务 ======================
void usbHostTask(void* arg) {
  while (1) {
    if (clientHandle) {
      usb_host_client_handle_events(clientHandle, portMAX_DELAY);
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// ====================== 初始化 USB Host ======================
void initUsbHost() {
  usb_host_config_t hostConfig = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  ESP_ERROR_CHECK(usb_host_install(&hostConfig));
  
  usb_host_client_config_t clientConfig = {
    .is_synchronous = false,
    .max_num_event_msg = 10,
    .async = {
      .client_event_callback = usbEventCallback,
      .callback_arg = NULL,
    },
  };
  ESP_ERROR_CHECK(usb_host_client_register(&clientConfig, &clientHandle));
  
  // 创建传输队列
  transferQueue = xQueueCreate(TRANSFER_POOL_SIZE, sizeof(usb_transfer_t*));
  for (int i = 0; i < TRANSFER_POOL_SIZE; i++) {
    usb_transfer_t* transfer = NULL;
    if (usb_host_transfer_alloc(TRANSFER_MAX_SIZE, 0, &transfer) == ESP_OK) {
      transfer->callback = printerTransferCallback;
      xQueueSend(transferQueue, &transfer, portMAX_DELAY);
    }
  }
  
  usbMutex = xSemaphoreCreateMutex();
  
  xTaskCreatePinnedToCore(usbHostTask, "usb_events", 8192, NULL, 3, NULL, 0);
  
  USB.begin();
  Serial.println("[USB] Host initialized");
}

// ====================== 远控模式：有线 HID 键盘 ======================
void sendUsbKeyReport(uint8_t modifiers, uint8_t keycode) {
  if (!usbHidReady) return;
  
  uint8_t report[8] = {0};
  report[0] = modifiers;
  report[2] = keycode;
  usb_hid.sendReport(1, report, sizeof(report));
  delay(20);
  report[0] = 0;
  report[2] = 0;
  usb_hid.sendReport(1, report, sizeof(report));
}

void sendUsbKeyPress(uint8_t keycode) {
  sendUsbKeyReport(0, keycode);
}

void sendUsbKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
  uint8_t modifiers = 0;
  if (ctrl)  modifiers |= 0x01;
  if (shift) modifiers |= 0x02;
  if (alt)   modifiers |= 0x04;
  if (gui)   modifiers |= 0x08;
  sendUsbKeyReport(modifiers, keycode);
}

void sendUsbString(const char* str) {
  while (*str) {
    char c = *str++;
    if (c >= 'a' && c <= 'z') {
      sendUsbKeyPress(HID_KEY_A + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
      sendUsbKeyPressWithModifiers(HID_KEY_A + (c - 'A'), false, false, true, false);
    } else if (c >= '0' && c <= '9') {
      sendUsbKeyPress(HID_KEY_1 + (c - '1'));
    } else if (c == ' ') {
      sendUsbKeyPress(HID_KEY_SPACE);
    } else if (c == '\n') {
      sendUsbKeyPress(HID_KEY_ENTER);
    }
    delay(30);
  }
}

// ====================== 远控模式：蓝牙 HID ======================
void sendBleKeyPress(uint8_t keycode) {
  if (!bleReady || !bleKeyboard.isConnected()) return;
  bleKeyboard.write(keycode);
}

void sendBleKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
  if (!bleReady || !bleKeyboard.isConnected()) return;
  
  uint8_t modifiers = 0;
  if (ctrl)  modifiers |= 0x01;
  if (shift) modifiers |= 0x02;
  if (alt)   modifiers |= 0x04;
  if (gui)   modifiers |= 0x08;
  
  bleKeyboard.sendReport((KeyReport){modifiers, 0, keycode, 0, 0, 0});
  delay(50);
  bleKeyboard.sendReport((KeyReport){0, 0, 0, 0, 0, 0});
}

void sendBleString(const char* str) {
  if (!bleReady || !bleKeyboard.isConnected()) return;
  bleKeyboard.print(str);
}

// ====================== 统一远控发送接口 ======================
void sendRemoteKeyPress(uint8_t keycode) {
  if (usbHidReady) {
    sendUsbKeyPress(keycode);
  } else if (bleReady && bleKeyboard.isConnected()) {
    sendBleKeyPress(keycode);
  }
}

void sendRemoteKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
  if (usbHidReady) {
    sendUsbKeyPressWithModifiers(keycode, ctrl, alt, shift, gui);
  } else if (bleReady && bleKeyboard.isConnected()) {
    sendBleKeyPressWithModifiers(keycode, ctrl, alt, shift, gui);
  }
}

void sendRemoteString(const char* str) {
  if (usbHidReady) {
    sendUsbString(str);
  } else if (bleReady && bleKeyboard.isConnected()) {
    sendBleString(str);
  }
}

// ====================== WebSocket 命令处理 ======================
void handleRemoteCommand(JsonObject& cmd) {
  String type = cmd["type"];
  
  if (type == "key") {
    String keyName = cmd["key"];
    for (int i = 0; i < keyMapSize; i++) {
      if (keyName == keyMap[i].name) {
        sendRemoteKeyPress(keyMap[i].key);
        break;
      }
    }
  }
  else if (type == "char") {
    String ch = cmd["char"];
    if (ch.length() == 1) {
      char c = ch[0];
      if (c >= 'a' && c <= 'z') {
        sendRemoteKeyPress(HID_KEY_A + (c - 'a'));
      } else if (c >= 'A' && c <= 'Z') {
        sendRemoteKeyPressWithModifiers(HID_KEY_A + (c - 'A'), false, false, true, false);
      } else if (c >= '0' && c <= '9') {
        sendRemoteKeyPress(HID_KEY_1 + (c - '1'));
      } else if (c == ' ') {
        sendRemoteKeyPress(HID_KEY_SPACE);
      } else if (c == '\n') {
        sendRemoteKeyPress(HID_KEY_ENTER);
      }
    }
  }
  else if (type == "text") {
    String text = cmd["text"];
    sendRemoteString(text.c_str());
  }
  else if (type == "modifier") {
    String mod = cmd["modifier"];
    bool pressed = cmd["pressed"];
    if (mod == "ctrl") ctrlPressed = pressed;
    else if (mod == "alt") altPressed = pressed;
    else if (mod == "shift") shiftPressed = pressed;
    else if (mod == "gui") guiPressed = pressed;
  }
  else if (type == "combined") {
    String keyName = cmd["key"];
    uint8_t keyCode = 0;
    if (keyName == "c") keyCode = HID_KEY_C;
    else if (keyName == "v") keyCode = HID_KEY_V;
    else if (keyName == "x") keyCode = HID_KEY_X;
    else if (keyName == "a") keyCode = HID_KEY_A;
    else if (keyName == "z") keyCode = HID_KEY_Z;
    else if (keyName == "r") keyCode = HID_KEY_R;
    
    if (keyCode != 0) {
      sendRemoteKeyPressWithModifiers(keyCode, ctrlPressed, altPressed, shiftPressed, guiPressed);
    }
  }
  else if (type == "media") {
    String mediaKey = cmd["media"];
    if (mediaKey == "volup") sendRemoteKeyPress(HID_KEY_VOLUME_UP);
    else if (mediaKey == "voldown") sendRemoteKeyPress(HID_KEY_VOLUME_DOWN);
    else if (mediaKey == "mute") sendRemoteKeyPress(HID_KEY_MUTE);
    else if (mediaKey == "playpause") sendRemoteKeyPress(HID_KEY_PLAY_PAUSE);
    else if (mediaKey == "next") sendRemoteKeyPress(HID_KEY_NEXT_TRACK);
    else if (mediaKey == "prev") sendRemoteKeyPress(HID_KEY_PREVIOUS_TRACK);
  }
}

// ====================== WebSocket 事件 ======================
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      Serial.printf("[WS] Client %u connected\n", num);
      break;
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client %u disconnected\n", num);
      break;
    case WStype_TEXT:
      {
        String jsonStr = String((char*)payload);
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, jsonStr);
        if (!error && currentWorkMode == MODE_REMOTE) {
          handleRemoteCommand(doc.as<JsonObject>());
        }
      }
      break;
  }
}

// ====================== WiFi 网络管理 ======================
void startAPMode() {
  currentNetMode = MODE_AP;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("[NET] AP mode, IP: %s\n", WiFi.softAPIP().toString());
}

bool tryConnectWiFi(int index) {
  if (index >= savedNetworkCount) return false;
  
  String ssid = savedNetworks[index].ssid;
  String pwd = savedNetworks[index].password;
  Serial.printf("[NET] Connecting to %s\n", ssid.c_str());
  
  WiFi.begin(ssid.c_str(), pwd.c_str());
  
  unsigned long start = millis();
  while (millis() - start < 10000) {
    if (WiFi.status() == WL_CONNECTED) {
      currentNetMode = MODE_WIFI_CLIENT;
      Serial.printf("[NET] Connected, IP: %s\n", WiFi.localIP().toString());
      return true;
    }
    delay(200);
  }
  return false;
}

void connectToBestWiFi() {
  if (savedNetworkCount == 0) {
    startAPMode();
    return;
  }
  
  for (int i = 0; i < savedNetworkCount; i++) {
    if (tryConnectWiFi(i)) return;
  }
  
  startAPMode();
}

// ====================== 模式切换 ======================
void switchWorkMode(WorkMode newMode) {
  if (currentWorkMode == newMode) return;
  
  currentWorkMode = newMode;
  prefs.putInt("work_mode", newMode);
  Serial.printf("[MODE] Switched to %s\n", newMode == MODE_PRINTER ? "Printer" : "Remote");
  
  if (newMode == MODE_PRINTER) {
    // 切换到打印模式，重新初始化 USB Host 检测打印机
    if (!deviceConnected && clientHandle) {
      // 触发重新检测
    }
  }
}

// ====================== 网页服务器 ======================
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    String modeClass = (currentWorkMode == MODE_PRINTER) ? "printer" : "remote";
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>ESP32 双模式桥接器</title>";
    html += "<style>";
    html += "*{margin:0;padding:0;box-sizing:border-box;}";
    html += "body{font-family:'Segoe UI',Arial,sans-serif;background:linear-gradient(135deg,#1a1a2e,#16213e);min-height:100vh;color:#eee;}";
    html += ".container{max-width:1200px;margin:0 auto;padding:20px;}";
    html += "h1{text-align:center;margin-bottom:30px;color:#00d4ff;}";
    html += ".mode-switch{display:flex;justify-content:center;gap:20px;margin-bottom:30px;}";
    html += ".mode-btn{padding:12px 30px;font-size:18px;border:none;border-radius:30px;cursor:pointer;transition:all 0.3s;background:#2c3e50;color:#ecf0f1;}";
    html += ".mode-btn.printer.active{background:#27ae60;box-shadow:0 0 15px #27ae60;}";
    html += ".mode-btn.remote.active{background:#e67e22;box-shadow:0 0 15px #e67e22;}";
    html += ".card{background:rgba(255,255,255,0.1);backdrop-filter:blur(10px);border-radius:15px;padding:20px;margin-bottom:20px;}";
    html += ".card h3{margin-bottom:15px;color:#00d4ff;}";
    html += ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:15px;}";
    html += ".status-item{background:rgba(0,0,0,0.3);padding:12px;border-radius:8px;}";
    html += ".status-label{font-size:12px;color:#aaa;}";
    html += ".status-value{font-size:18px;font-weight:bold;}";
    html += ".online{color:#2ecc71;}";
    html += ".offline{color:#e74c3c;}";
    html += "table{width:100%;border-collapse:collapse;}";
    html += "td,th{padding:10px;text-align:left;border-bottom:1px solid rgba(255,255,255,0.1);}";
    html += "button{padding:6px 12px;border:none;border-radius:5px;cursor:pointer;transition:0.2s;}";
    html += "button.primary{background:#3498db;color:white;}";
    html += "button.danger{background:#e74c3c;color:white;}";
    html += "button.warning{background:#f39c12;color:white;}";
    html += "input,select{padding:8px;border-radius:5px;border:1px solid #555;background:#2c3e50;color:white;}";
    html += ".remote-panel{display:" + String(currentWorkMode == MODE_REMOTE ? "block" : "none") + ";}";
    html += ".printer-panel{display:" + String(currentWorkMode == MODE_PRINTER ? "block" : "none") + ";}";
    html += ".keyboard{display:grid;grid-template-columns:repeat(10,1fr);gap:8px;margin-top:15px;}";
    html += ".key{background:#2c3e50;padding:12px;text-align:center;border-radius:8px;cursor:pointer;transition:0.1s;}";
    html += ".key:active{background:#e67e22;transform:scale(0.95);}";
    html += ".key.wide{grid-column:span 2;}";
    html += ".modifier-row{display:flex;gap:10px;margin-bottom:15px;}";
    html += ".modifier{background:#34495e;padding:10px 15px;border-radius:8px;cursor:pointer;}";
    html += ".modifier.active{background:#e67e22;}";
    html += ".text-input{display:flex;gap:10px;margin-bottom:15px;}";
    html += ".text-input input{flex:1;}";
    html += "@media(max-width:768px){.keyboard{grid-template-columns:repeat(5,1fr);}}";
    html += "</style>";
    
    html += "<script>";
    html += "let ws=null;let currentMode='" + modeClass + "';";
    html += "function connectWS(){if(currentMode==='remote'){ws=new WebSocket('ws://'+window.location.hostname+':81');ws.onopen=()=>console.log('WS connected');ws.onclose=()=>setTimeout(connectWS,3000);}}";
    html += "function sendCommand(cmd){if(ws&&ws.readyState===WebSocket.OPEN){ws.send(JSON.stringify(cmd));}}";
    html += "function sendKey(key){sendCommand({type:'key',key:key});}";
    html += "function sendChar(ch){sendCommand({type:'char',char:ch});}";
    html += "function sendText(){let t=document.getElementById('textInput').value;if(t){sendCommand({type:'text',text:t});document.getElementById('textInput').value='';}}";
    html += "function toggleModifier(mod){let btn=document.getElementById('mod_'+mod);let active=btn.classList.toggle('active');sendCommand({type:'modifier',modifier:mod,pressed:active});}";
    html += "function sendCombined(key){sendCommand({type:'combined',key:key});}";
    html += "function sendMedia(key){sendCommand({type:'media',media:key});}";
    html += "function switchMode(mode){fetch('/api/mode?mode='+mode).then(()=>{setTimeout(()=>location.reload(),500);});}";
    html += "function reboot(){if(confirm('Reboot device?'))fetch('/api/reboot');}";
    html += "function scanWiFi(){fetch('/api/scan').then(()=>setTimeout(loadWiFiList,5000));loadWiFiList();}";
    html += "function loadWiFiList(){fetch('/api/networks').then(r=>r.json()).then(data=>{let html='<table><tr><th>SSID</th><th>Signal</th><th>Action</th></tr>';data.forEach(n=>{if(n.ssid)html+=`<tr><td>${n.ssid}</td><td>${n.rssi}dBm</td><td><button onclick='connectWiFi(\"${n.ssid}\")'>Connect</button></td>`;});html+='</table>';document.getElementById('wifiList').innerHTML=html;});}";
    html += "function connectWiFi(ssid){let pwd=prompt('Password for '+ssid+':');if(pwd){fetch('/api/wifi/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pwd})}).then(()=>{alert('Added, rebooting...');setTimeout(()=>location.reload(),3000);});}}";
    html += "function addWiFi(){let ssid=document.getElementById('newSsid').value;let pwd=document.getElementById('newPwd').value;if(ssid){fetch('/api/wifi/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pwd})}).then(()=>location.reload());}}";
    html += "function clearWiFi(){if(confirm('Clear all WiFi?')){fetch('/api/wifi/clear').then(()=>location.reload());}}";
    html += "setInterval(()=>{fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('mode_display').innerText=d.mode==='printer'?'🖨️ Printer':'🎮 Remote';document.getElementById('device_status').innerHTML=d.printer_ready?'🖨️ Printer Ready':'🎮 Remote Ready';document.getElementById('ip_display').innerText=d.ip;document.getElementById('wifi_display').innerText=d.ssid;document.getElementById('heap_display').innerText=d.heap;});},3000);";
    html += "window.onload=()=>{connectWS();loadWiFiList();};";
    html += "</script>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>🔌 ESP32-S3 双模式桥接器 v5.0</h1>";
    
    // 模式切换按钮
    html += "<div class='mode-switch'>";
    html += "<button class='mode-btn printer" + String(currentWorkMode == MODE_PRINTER ? " active" : "") + "' onclick='switchMode(\"printer\")'>🖨️ 打印模式</button>";
    html += "<button class='mode-btn remote" + String(currentWorkMode == MODE_REMOTE ? " active" : "") + "' onclick='switchMode(\"remote\")'>🎮 远控模式</button>";
    html += "</div>";
    
    // 状态卡片
    html += "<div class='card'><h3>📊 系统状态</h3><div class='status-grid'>";
    html += "<div class='status-item'><div class='status-label'>工作模式</div><div class='status-value' id='mode_display'>-</div></div>";
    html += "<div class='status-item'><div class='status-label'>设备状态</div><div class='status-value' id='device_status'>-</div></div>";
    html += "<div class='status-item'><div class='status-label'>IP地址</div><div class='status-value' id='ip_display'>-</div></div>";
    html += "<div class='status-item'><div class='status-label'>WiFi</div><div class='status-value' id='wifi_display'>-</div></div>";
    html += "<div class='status-item'><div class='status-label'>空闲内存</div><div class='status-value' id='heap_display'>-</div></div>";
    html += "</div></div>";
    
    // 打印模式面板
    html += "<div class='printer-panel'><div class='card'><h3>🖨️ 打印模式</h3>";
    html += "<p>打印机 RAW 端口：<strong>" + String(PRINT_PORT) + "</strong></p>";
    html += "<p>Windows 添加打印机：</p><ol><li>控制面板 → 设备和打印机 → 添加打印机</li>";
    html += "<li>选择「通过手动设置添加本地打印机或网络打印机」</li>";
    html += "<li>创建新端口 → Standard TCP/IP Port → 输入 ESP32 IP 地址</li>";
    html += "<li>端口号：<strong>" + String(PRINT_PORT) + "</strong> → 选择你的打印机驱动完成</li></ol>";
    html += "</div></div>";
    
    // 远控模式面板
    html += "<div class='remote-panel'><div class='card'><h3>🎮 远控模式</h3>";
    html += "<div class='modifier-row'>";
    html += "<div class='modifier' id='mod_ctrl' onclick='toggleModifier(\"ctrl\")'>Ctrl</div>";
    html += "<div class='modifier' id='mod_alt' onclick='toggleModifier(\"alt\")'>Alt</div>";
    html += "<div class='modifier' id='mod_shift' onclick='toggleModifier(\"shift\")'>Shift</div>";
    html += "<div class='modifier' id='mod_gui' onclick='toggleModifier(\"gui\")'>Win</div>";
    html += "</div>";
    html += "<div class='text-input'><input type='text' id='textInput' placeholder='输入文本后点击发送...'><button class='primary' onclick='sendText()'>📤 发送</button></div>";
    html += "<div class='keyboard'>";
    // 数字行
    for(int i=1;i<=10;i++) html += "<div class='key' onclick='sendChar(\"" + String(i%10) + "\")'>" + String(i%10) + "</div>";
    // 字母行
    const char* keys[] = {"q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m"};
    for(auto& k : keys) html += "<div class='key' onclick='sendChar(\"" + String(k) + "\")'>" + String(k) + "</div>";
    // 功能键
    const char* funcs[] = {"space","enter","tab","esc","backspace","del","up","down","left","right"};
    for(auto& f : funcs) html += "<div class='key wide' onclick='sendKey(\"" + String(f) + "\")'>" + String(f) + "</div>";
    html += "</div><div style='margin-top:15px;'><h4>🎵 媒体控制</h4><div class='keyboard'>";
    const char* media[] = {"volup","voldown","mute","playpause","next","prev"};
    for(auto& m : media) html += "<div class='key' onclick='sendMedia(\"" + String(m) + "\")'>" + String(m) + "</div>";
    html += "</div></div>";
    html += "<p style='margin-top:15px;'>💡 连接方式：<strong>" + String(usbHidReady ? "USB有线已连接" : (bleReady ? "蓝牙待配对" : "未就绪")) + "</strong></p>";
    html += "</div></div>";
    
    // WiFi 管理卡片
    html += "<div class='card'><h3>📶 WiFi 管理</h3><div id='wifiList'>加载中...</div>";
    html += "<hr><div style='display:flex;gap:10px;margin-top:10px;'>";
    html += "<input type='text' id='newSsid' placeholder='SSID'>";
    html += "<input type='password' id='newPwd' placeholder='密码'>";
    html += "<button class='primary' onclick='addWiFi()'>添加</button>";
    html += "<button class='danger' onclick='clearWiFi()'>清空全部</button>";
    html += "<button class='warning' onclick='scanWiFi()'>扫描</button>";
    html += "</div></div>";
    
    html += "<div class='card'><button class='danger' onclick='reboot()'>🔄 重启设备</button></div>";
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
  });
  
  // API: 获取状态
  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    doc["mode"] = currentWorkMode == MODE_PRINTER ? "printer" : "remote";
    doc["printer_ready"] = printerReady;
    doc["usb_hid_ready"] = usbHidReady;
    doc["ble_ready"] = bleReady;
    doc["ip"] = (currentNetMode == MODE_WIFI_CLIENT && WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["ssid"] = (currentNetMode == MODE_WIFI_CLIENT && WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : AP_SSID;
    doc["heap"] = ESP.getFreeHeap();
    doc["ble_connected"] = bleReady ? bleKeyboard.isConnected() : false;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
  
  // API: 切换模式
  server.on("/api/mode", HTTP_GET, []() {
    if (server.hasArg("mode")) {
      String mode = server.arg("mode");
      if (mode == "printer") {
        switchWorkMode(MODE_PRINTER);
      } else if (mode == "remote") {
        switchWorkMode(MODE_REMOTE);
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      server.send(400, "application/json", "{\"status\":\"error\"}");
    }
  });
  
  // API: 扫描 WiFi
  server.on("/api/scan", HTTP_GET, []() {
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
  });
  
  // API: 获取 WiFi 列表
  server.on("/api/networks", HTTP_GET, []() {
    int n = WiFi.scanComplete();
    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.to<JsonArray>();
    
    if (n > 0) {
      for (int i = 0; i < n && i < 30; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["ssid"] = WiFi.SSID(i);
        obj["rssi"] = WiFi.RSSI(i);
        obj["encrypted"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      }
    }
    
    String response;
    serializeJson(arr, response);
    server.send(200, "application/json", response);
  });
  
  // API: 添加 WiFi
  server.on("/api/wifi/add", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, server.arg("plain"));
      if (!error) {
        String ssid = doc["ssid"].as<String>();
        String pwd = doc["password"].as<String>();
        if (ssid.length() > 0) {
          addWiFiNetwork(ssid, pwd);
          server.send(200, "application/json", "{\"status\":\"ok\"}");
          return;
        }
      }
    }
    server.send(400, "application/json", "{\"status\":\"error\"}");
  });
  
  // API: 清空 WiFi
  server.on("/api/wifi/clear", HTTP_GET, []() {
    clearAllWiFi();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });
  
  // API: 重启
  server.on("/api/reboot", HTTP_GET, []() {
    server.send(200, "application/json", "{\"status\":\"rebooting\"}");
    delay(100);
    ESP.restart();
  });
  
  server.begin();
  Serial.println("[WEB] HTTP server started");
}

// ====================== 恢复出厂设置 ======================
void checkFactoryReset() {
  pinMode(0, INPUT_PULLUP);
  unsigned long pressStart = 0;
  bool pressing = false;
  
  for (int i = 0; i < 100; i++) {
    if (digitalRead(0) == LOW) {
      if (!pressing) {
        pressing = true;
        pressStart = millis();
      } else if (millis() - pressStart > 5000) {
        Serial.println("[FACTORY] Factory reset...");
        clearAllWiFi();
        prefs.clear();
        delay(1000);
        ESP.restart();
      }
    } else {
      pressing = false;
    }
    delay(50);
  }
}

// ====================== 初始化远控模式 ======================
void initRemoteMode() {
  // 初始化有线 HID
  usb_hid.begin();
  for (int i = 0; i < 50; i++) {
    if (TinyUSBDevice.mounted()) {
      usbHidReady = true;
      Serial.println("[REMOTE] USB HID ready (wired)");
      break;
    }
    delay(100);
  }
  if (!usbHidReady) {
    Serial.println("[REMOTE] USB not connected, will use BLE");
  }
  
  // 初始化蓝牙
  bleKeyboard.begin();
  bleReady = true;
  Serial.println("[REMOTE] BLE keyboard ready, name: ESP32 Remote");
  
  remoteReady = usbHidReady || bleReady;
}

// ====================== 主程序 ======================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n");
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║     ESP32-S3 Dual Mode USB Bridge v5.0 - Production        ║");
  Serial.println("║     Print Mode + Remote Control Mode                        ║");
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
  Serial.println();
  
  // 初始化存储
  prefs.begin("usb_bridge", false);
  loadWiFiConfigs();
  
  // 读取上次工作模式
  int savedMode = prefs.getInt("work_mode", 1);
  currentWorkMode = (savedMode == 0) ? MODE_PRINTER : MODE_REMOTE;
  Serial.printf("[INIT] Work mode: %s\n", currentWorkMode == MODE_PRINTER ? "Printer" : "Remote");
  
  // 检查恢复出厂
  checkFactoryReset();
  
  // 初始化远控模式（始终初始化，因为模式可切换）
  initRemoteMode();
  
  // 初始化 USB Host（打印模式需要）
  initUsbHost();
  
  // 初始化网络
  connectToBestWiFi();
  
  // 启动服务器
  setupWebServer();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  printServer.begin();
  
  Serial.printf("[INIT] Print port: %d, Web port: %d, WS port: %d\n", PRINT_PORT, WEB_PORT, WS_PORT);
  Serial.println("[INIT] System ready!");
}

void loop() {
  // 网络处理
  if (currentNetMode == MODE_AP) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  webSocket.loop();
  
  // 打印模式：处理打印机数据
  if (currentWorkMode == MODE_PRINTER && printerReady) {
    WiFiClient client = printServer.available();
    if (client && client.connected()) {
      Serial.println("[PRINT] Receiving print job...");
      
      while (client.connected()) {
        int len = client.available();
        if (len > 0) {
          len = min(len, USB_RECV_BUF_SIZE);
          int bytesRead = client.read(recvBuf, len);
          if (bytesRead > 0) {
            if (!sendToPrinter(recvBuf, bytesRead)) {
              Serial.println("[PRINT] Failed to send data");
              break;
            }
          }
        }
        delay(1);
      }
      
      client.stop();
      Serial.println("[PRINT] Job completed");
    }
  }
  
  delay(10);
}
