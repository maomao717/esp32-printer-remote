/**
 * ESP32-S3 双模式 USB 桥接器 v5.2 - 完整修正版
 * 
 * 模式1 - 打印模式：USB打印机 → 网络打印（RAW端口9100）
 * 模式2 - 远控模式：模拟键盘（USB有线 + 蓝牙无线）远程控制电脑
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
#include <Adafruit_TinyUSB.h>
#include <BleKeyboard.h>

// ====================== 宏定义补充 ======================
#ifndef USB_B_DESCRIPTOR_TYPE_ENDPOINT
#define USB_B_DESCRIPTOR_TYPE_ENDPOINT 0x05
#endif

#ifndef USB_B_DESCRIPTOR_TYPE_INTERFACE
#define USB_B_DESCRIPTOR_TYPE_INTERFACE 0x04
#endif

// ====================== HID 键码补充定义 ======================
#ifndef HID_KEY_0
#define HID_KEY_0 0x27
#endif
#ifndef HID_KEY_VOLUME_UP
#define HID_KEY_VOLUME_UP       0x80
#endif
#ifndef HID_KEY_VOLUME_DOWN
#define HID_KEY_VOLUME_DOWN     0x81
#endif
#ifndef HID_KEY_MUTE
#define HID_KEY_MUTE            0x7F
#endif
#ifndef HID_KEY_MEDIA_PLAY_PAUSE
#define HID_KEY_MEDIA_PLAY_PAUSE 0xCD
#endif
#ifndef HID_KEY_MEDIA_NEXT_TRACK
#define HID_KEY_MEDIA_NEXT_TRACK 0xB5
#endif
#ifndef HID_KEY_MEDIA_PREVIOUS_TRACK
#define HID_KEY_MEDIA_PREVIOUS_TRACK 0xB6
#endif

// ====================== USB HID 报告描述符 ======================
static const uint8_t keyboard_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1))
};

// ====================== 配置常量 ======================
const char* AP_SSID           = "ESP32-Bridge";
const char* AP_PASSWORD       = "12345678";
const int   PRINT_PORT        = 9100;
const int   WEB_PORT          = 80;
const int   WS_PORT           = 81;
const int   MAX_WIFI_NETWORKS = 5;

enum WorkMode { MODE_PRINTER, MODE_REMOTE };
enum NetMode { MODE_WIFI_CLIENT, MODE_AP };

volatile WorkMode currentWorkMode = MODE_REMOTE;
NetMode currentNetMode = MODE_WIFI_CLIENT;

volatile bool printerReady = false;
volatile bool deviceConnected = false;
volatile bool remoteReady = false;
volatile bool usbHidReady = false;
volatile bool bleReady = false;

Preferences prefs;
WebServer server(WEB_PORT);
WebSocketsServer webSocket = WebSocketsServer(WS_PORT);
DNSServer dnsServer;

struct WiFiConfig { String ssid; String password; };
WiFiConfig savedNetworks[MAX_WIFI_NETWORKS];
int savedNetworkCount = 0;

WiFiServer printServer(PRINT_PORT);
static usb_host_client_handle_t clientHandle = NULL;
static usb_device_handle_t      deviceHandle = NULL;
static uint8_t                  bulkOutEpAddr = 0;
static uint16_t                 bulkOutEpMps  = 64;
static uint8_t                  printerInterfaceNum = 0;

#define TRANSFER_MAX_SIZE 1024
#define TRANSFER_POOL_SIZE 6
static QueueHandle_t transferQueue = NULL;
static SemaphoreHandle_t usbMutex = NULL;
static SemaphoreHandle_t hidMutex = NULL;
#define USB_RECV_BUF_SIZE 4096
static uint8_t recvBuf[USB_RECV_BUF_SIZE];

Adafruit_USBD_HID usb_hid;
BleKeyboard bleKeyboard("ESP32 Remote", "Espressif", 100);

volatile bool ctrlPressed = false;
volatile bool altPressed = false;
volatile bool shiftPressed = false;
volatile bool guiPressed = false;

struct KeyMapping { const char* name; uint8_t key; };
KeyMapping keyMap[] = {
    {"enter", HID_KEY_ENTER}, {"space", HID_KEY_SPACE}, {"tab", HID_KEY_TAB},
    {"esc", HID_KEY_ESCAPE}, {"backspace", HID_KEY_BACKSPACE}, {"del", HID_KEY_DELETE},
    {"up", HID_KEY_ARROW_UP}, {"down", HID_KEY_ARROW_DOWN},
    {"left", HID_KEY_ARROW_LEFT}, {"right", HID_KEY_ARROW_RIGHT},
    {"home", HID_KEY_HOME}, {"end", HID_KEY_END},
    {"pgup", HID_KEY_PAGE_UP}, {"pgdn", HID_KEY_PAGE_DOWN},
    {"f1", HID_KEY_F1}, {"f2", HID_KEY_F2}, {"f3", HID_KEY_F3}, {"f4", HID_KEY_F4},
    {"f5", HID_KEY_F5}, {"f6", HID_KEY_F6}, {"f7", HID_KEY_F7}, {"f8", HID_KEY_F8},
    {"f9", HID_KEY_F9}, {"f10", HID_KEY_F10}, {"f11", HID_KEY_F11}, {"f12", HID_KEY_F12}
};
int keyMapSize = sizeof(keyMap) / sizeof(keyMap[0]);

// ====================== WiFi 配置 ======================
void loadWiFiConfigs() {
    savedNetworkCount = prefs.getInt("wifi_count", 0);
    if (savedNetworkCount > MAX_WIFI_NETWORKS) savedNetworkCount = MAX_WIFI_NETWORKS;
    for (int i = 0; i < savedNetworkCount; i++) {
        savedNetworks[i].ssid = prefs.getString(("wifi_" + String(i) + "_ssid").c_str(), "");
        savedNetworks[i].password = prefs.getString(("wifi_" + String(i) + "_pwd").c_str(), "");
    }
}

void saveWiFiConfigs() {
    prefs.putInt("wifi_count", savedNetworkCount);
    for (int i = 0; i < savedNetworkCount; i++) {
        prefs.putString(("wifi_" + String(i) + "_ssid").c_str(), savedNetworks[i].ssid);
        prefs.putString(("wifi_" + String(i) + "_pwd").c_str(), savedNetworks[i].password);
    }
}

void addWiFiNetwork(String ssid, String password) {
    for (int i = 0; i < savedNetworkCount; i++)
        if (savedNetworks[i].ssid == ssid) { 
            savedNetworks[i].password = password; 
            saveWiFiConfigs(); 
            return; 
        }
    if (savedNetworkCount < MAX_WIFI_NETWORKS) {
        savedNetworks[savedNetworkCount].ssid = ssid;
        savedNetworks[savedNetworkCount].password = password;
        savedNetworkCount++; 
        saveWiFiConfigs();
    }
}

void clearAllWiFi() { 
    savedNetworkCount = 0; 
    saveWiFiConfigs(); 
}

// ====================== USB 传输回调 ======================
void printerTransferCallback(usb_transfer_t* transfer) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (transfer->status == USB_TRANSFER_STATUS_STALL) {
        if (clientHandle && bulkOutEpAddr) {
            usb_host_endpoint_halt(deviceHandle, bulkOutEpAddr);
            usb_host_endpoint_flush(deviceHandle, bulkOutEpAddr);
        }
    }
    xQueueSendFromISR(transferQueue, &transfer, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

// ====================== 发送打印数据 ======================
bool sendToPrinter(const uint8_t* data, size_t len) {
    if (!printerReady || deviceHandle == NULL || bulkOutEpAddr == 0) return false;
    if (xSemaphoreTake(usbMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    size_t sent = 0; 
    bool success = true;
    
    while (sent < len) {
        size_t chunk = min((size_t)bulkOutEpMps, len - sent);
        usb_transfer_t* transfer = NULL;
        
        if (xQueueReceive(transferQueue, &transfer, pdMS_TO_TICKS(500)) != pdTRUE) { 
            success = false; 
            break; 
        }
        
        transfer->num_bytes = chunk; 
        transfer->device_handle = deviceHandle;
        transfer->bEndpointAddress = bulkOutEpAddr; 
        transfer->callback = printerTransferCallback;
        memcpy(transfer->data_buffer, data + sent, chunk);
        
        if (usb_host_transfer_submit(transfer) != ESP_OK) { 
            xQueueSend(transferQueue, &transfer, portMAX_DELAY); 
            success = false; 
            break; 
        }
        
        sent += chunk; 
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
    if (success && len > 0 && (len % bulkOutEpMps) == 0) {
        usb_transfer_t* zlpTransfer = NULL;
        if (xQueueReceive(transferQueue, &zlpTransfer, pdMS_TO_TICKS(500)) == pdTRUE) {
            zlpTransfer->num_bytes = 0; 
            zlpTransfer->device_handle = deviceHandle;
            zlpTransfer->bEndpointAddress = bulkOutEpAddr; 
            zlpTransfer->callback = printerTransferCallback;
            usb_host_transfer_submit(zlpTransfer);
        }
    }
    
    xSemaphoreGive(usbMutex); 
    return success;
}

// ====================== 初始化打印机 ======================
bool initPrinterDevice(uint8_t devAddr) {
    if (usb_host_device_open(clientHandle, devAddr, &deviceHandle) != ESP_OK) return false;
    
    const usb_device_desc_t* devDesc;
    usb_host_get_device_descriptor(deviceHandle, &devDesc);
    
    const usb_config_desc_t* cfgDesc;
    usb_host_get_active_config_descriptor(deviceHandle, &cfgDesc);
    
    uint8_t* descPtr = (uint8_t*)cfgDesc;
    uint16_t totalLen = cfgDesc->wTotalLength;
    uint16_t parsed = 0;
    
    bool printerFound = false;
    
    while (parsed < totalLen) {
        uint8_t* desc = descPtr + parsed;
        uint8_t len = desc[0];
        uint8_t type = desc[1];
        
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            usb_intf_desc_t* intf = (usb_intf_desc_t*)desc;
            if (intf->bInterfaceClass == 0x07) {  // 打印机类
                printerFound = true;
                printerInterfaceNum = intf->bInterfaceNumber;
            }
        }
        else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && printerFound) {
            usb_ep_desc_t* ep = (usb_ep_desc_t*)desc;
            if ((ep->bmAttributes & 0x03) == USB_TRANSFER_TYPE_BULK && 
                (ep->bEndpointAddress & 0x80) == 0) {
                bulkOutEpAddr = ep->bEndpointAddress;
                bulkOutEpMps = ep->wMaxPacketSize;
                break;
            }
        }
        parsed += len;
    }
    
    if (!printerFound || bulkOutEpAddr == 0) {
        usb_host_device_close(clientHandle, deviceHandle);
        deviceHandle = NULL;
        return false;
    }
    
    if (usb_host_interface_claim(clientHandle, deviceHandle, printerInterfaceNum, 0) != ESP_OK) {
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
            if (currentWorkMode == MODE_PRINTER && !deviceConnected) {
                if (initPrinterDevice(msg->new_dev.address)) { 
                    deviceConnected = true; 
                    printerReady = true; 
                    Serial.println("[USB] Printer connected");
                }
            }
            break;
            
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            if (deviceHandle) { 
                usb_host_interface_release(clientHandle, deviceHandle, printerInterfaceNum); 
                usb_host_device_close(clientHandle, deviceHandle); 
                deviceHandle = NULL; 
            }
            deviceConnected = false; 
            printerReady = false; 
            bulkOutEpAddr = 0;
            Serial.println("[USB] Printer disconnected");
            break;
    }
}

// ====================== USB Host 任务 ======================
void usbHostTask(void* arg) {
    while (1) {
        if (clientHandle) {
            // 处理 USB 事件，使用超时避免永久阻塞
            usb_host_client_handle_events(clientHandle, pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ====================== 初始化 USB Host ======================
void initUsbHost() {
    usb_host_config_t hostConfig = { 
        .skip_phy_setup = false, 
        .intr_flags = ESP_INTR_FLAG_LEVEL1 
    };
    ESP_ERROR_CHECK(usb_host_install(&hostConfig));
    
    usb_host_client_config_t clientConfig = { 
        .is_synchronous = false, 
        .max_num_event_msg = 10, 
        .async = { 
            .client_event_callback = usbEventCallback, 
            .callback_arg = NULL 
        } 
    };
    ESP_ERROR_CHECK(usb_host_client_register(&clientConfig, &clientHandle));
    
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

void deinitUsbHost() {
    if (clientHandle) { 
        usb_host_client_deregister(clientHandle); 
        clientHandle = NULL; 
    }
    usb_host_uninstall();
    if (transferQueue) { 
        vQueueDelete(transferQueue); 
        transferQueue = NULL; 
    }
    if (usbMutex) { 
        vSemaphoreDelete(usbMutex); 
        usbMutex = NULL; 
    }
    printerReady = false; 
    deviceConnected = false;
    Serial.println("[USB] Host deinitialized");
}

// ====================== 有线 HID ======================
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
    if (ctrl) modifiers |= 0x01; 
    if (shift) modifiers |= 0x02;
    if (alt) modifiers |= 0x04; 
    if (gui) modifiers |= 0x08;
    sendUsbKeyReport(modifiers, keycode);
}

void sendUsbString(const char* str) {
    while (*str) {
        char c = *str++;
        if (c >= 'a' && c <= 'z') sendUsbKeyPress(HID_KEY_A + (c - 'a'));
        else if (c >= 'A' && c <= 'Z') sendUsbKeyPressWithModifiers(HID_KEY_A + (c - 'A'), false, false, true, false);
        else if (c >= '1' && c <= '9') sendUsbKeyPress(HID_KEY_1 + (c - '1'));
        else if (c == '0') sendUsbKeyPress(HID_KEY_0);
        else if (c == ' ') sendUsbKeyPress(HID_KEY_SPACE);
        else if (c == '\n') sendUsbKeyPress(HID_KEY_ENTER);
        delay(30);
    }
}

// ====================== 蓝牙 HID ======================
void sendBleKeyPress(uint8_t keycode) { 
    if (bleReady && bleKeyboard.isConnected()) {
        bleKeyboard.write(keycode);
    }
}

void sendBleKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
    if (!bleReady || !bleKeyboard.isConnected()) return;
    
    uint8_t modifiers = 0;
    if (ctrl) modifiers |= 0x01; 
    if (shift) modifiers |= 0x02;
    if (alt) modifiers |= 0x04; 
    if (gui) modifiers |= 0x08;
    
    uint8_t report[8] = {modifiers, 0, keycode, 0, 0, 0, 0, 0};
    bleKeyboard.sendReport((KeyReport*)report);
    delay(50);
    uint8_t emptyReport[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bleKeyboard.sendReport((KeyReport*)emptyReport);
}

void sendBleString(const char* str) { 
    if (bleReady && bleKeyboard.isConnected()) {
        bleKeyboard.print(str);
    }
}

// ====================== 统一发送接口 ======================
void sendRemoteKeyPress(uint8_t keycode) {
    if (xSemaphoreTake(hidMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (usbHidReady) {
            sendUsbKeyPress(keycode);
        } else if (bleReady && bleKeyboard.isConnected()) {
            sendBleKeyPress(keycode);
        }
        xSemaphoreGive(hidMutex);
    }
}

void sendRemoteKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
    if (xSemaphoreTake(hidMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (usbHidReady) {
            sendUsbKeyPressWithModifiers(keycode, ctrl, alt, shift, gui);
        } else if (bleReady && bleKeyboard.isConnected()) {
            sendBleKeyPressWithModifiers(keycode, ctrl, alt, shift, gui);
        }
        xSemaphoreGive(hidMutex);
    }
}

void sendRemoteString(const char* str) {
    if (xSemaphoreTake(hidMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (usbHidReady) {
            sendUsbString(str);
        } else if (bleReady && bleKeyboard.isConnected()) {
            sendBleString(str);
        }
        xSemaphoreGive(hidMutex);
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
            if (c >= 'a' && c <= 'z') sendRemoteKeyPress(HID_KEY_A + (c - 'a'));
            else if (c >= 'A' && c <= 'Z') sendRemoteKeyPressWithModifiers(HID_KEY_A + (c - 'A'), false, false, true, false);
            else if (c >= '1' && c <= '9') sendRemoteKeyPress(HID_KEY_1 + (c - '1'));
            else if (c == '0') sendRemoteKeyPress(HID_KEY_0);
            else if (c == ' ') sendRemoteKeyPress(HID_KEY_SPACE);
            else if (c == '\n') sendRemoteKeyPress(HID_KEY_ENTER);
        }
    } 
    else if (type == "text") { 
        sendRemoteString(cmd["text"].as<String>().c_str()); 
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
        if (keyCode) sendRemoteKeyPressWithModifiers(keyCode, ctrlPressed, altPressed, shiftPressed, guiPressed);
    } 
    else if (type == "media") {
        String mediaKey = cmd["media"];
        if (mediaKey == "volup") sendRemoteKeyPress(HID_KEY_VOLUME_UP);
        else if (mediaKey == "voldown") sendRemoteKeyPress(HID_KEY_VOLUME_DOWN);
        else if (mediaKey == "mute") sendRemoteKeyPress(HID_KEY_MUTE);
        else if (mediaKey == "playpause") sendRemoteKeyPress(HID_KEY_MEDIA_PLAY_PAUSE);
        else if (mediaKey == "next") sendRemoteKeyPress(HID_KEY_MEDIA_NEXT_TRACK);
        else if (mediaKey == "prev") sendRemoteKeyPress(HID_KEY_MEDIA_PREVIOUS_TRACK);
    }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_TEXT && currentWorkMode == MODE_REMOTE) {
        StaticJsonDocument<512> doc;
        if (!deserializeJson(doc, String((char*)payload))) {
            JsonObject obj = doc.as<JsonObject>(); 
            handleRemoteCommand(obj);
        }
    }
}

// ====================== WiFi 网络管理 ======================
void startAPMode() { 
    currentNetMode = MODE_AP; 
    WiFi.mode(WIFI_AP); 
    WiFi.softAP(AP_SSID, AP_PASSWORD); 
    dnsServer.start(53, "*", WiFi.softAPIP()); 
    Serial.println("[WiFi] AP mode started");
}

bool tryConnectWiFi(int index) {
    if (index >= savedNetworkCount) return false;
    
    WiFi.begin(savedNetworks[index].ssid.c_str(), savedNetworks[index].password.c_str());
    unsigned long start = millis();
    
    while (millis() - start < 10000) { 
        if (WiFi.status() == WL_CONNECTED) { 
            currentNetMode = MODE_WIFI_CLIENT; 
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
        if (tryConnectWiFi(i)) {
            Serial.printf("[WiFi] Connected to %s, IP: %s\n", 
                        savedNetworks[i].ssid.c_str(), 
                        WiFi.localIP().toString().c_str());
            return;
        }
    }
    startAPMode();
}

// ====================== 模式切换 ======================
void switchWorkMode(WorkMode newMode) {
    if (currentWorkMode == newMode) return;
    
    Serial.printf("[Mode] Switching from %d to %d\n", currentWorkMode, newMode);
    
    if (currentWorkMode == MODE_PRINTER) {
        deinitUsbHost();
    } else {
        // 断开当前 HID 连接
        if (bleReady && bleKeyboard.isConnected()) {
            bleKeyboard.end();
            delay(100);
        }
        usbHidReady = false;
    }
    
    if (newMode == MODE_PRINTER) {
        initUsbHost();
    } else {
        initRemoteMode();
    }
    
    currentWorkMode = newMode; 
    prefs.putInt("work_mode", newMode);
}

// ====================== 初始化远控模式 ======================
void initRemoteMode() {
    hidMutex = xSemaphoreCreateMutex();
    
    usb_hid.setReportDescriptor(keyboard_report_desc, sizeof(keyboard_report_desc));
    usb_hid.begin();
    
    for (int i = 0; i < 50; i++) { 
        if (TinyUSBDevice.mounted()) { 
            usbHidReady = true; 
            Serial.println("[HID] USB HID ready");
            break; 
        } 
        delay(100); 
    }
    
    bleKeyboard.begin(); 
    bleReady = true;
    remoteReady = usbHidReady || bleReady;
    
    Serial.println("[Remote] Remote mode initialized");
}

// ====================== 网页服务器 ======================
void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>ESP32 双模式桥接器 v5.2</title><style>";
        html += "*{margin:0;padding:0;box-sizing:border-box;}";
        html += "body{font-family:'Segoe UI',Arial,sans-serif;background:linear-gradient(135deg,#1a1a2e,#16213e);min-height:100vh;color:#eee;}";
        html += ".container{max-width:1200px;margin:0 auto;padding:20px;}h1{text-align:center;margin-bottom:30px;color:#00d4ff;}";
        html += ".mode-switch{display:flex;justify-content:center;gap:20px;margin-bottom:30px;}";
        html += ".mode-btn{padding:12px 30px;font-size:18px;border:none;border-radius:30px;cursor:pointer;transition:all 0.3s;background:#2c3e50;color:#ecf0f1;}";
        html += ".mode-btn.printer.active{background:#27ae60;box-shadow:0 0 15px #27ae60;}";
        html += ".mode-btn.remote.active{background:#e67e22;box-shadow:0 0 15px #e67e22;}";
        html += ".card{background:rgba(255,255,255,0.1);backdrop-filter:blur(10px);border-radius:15px;padding:20px;margin-bottom:20px;}";
        html += ".card h3{margin-bottom:15px;color:#00d4ff;}";
        html += ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:15px;}";
        html += ".status-item{background:rgba(0,0,0,0.3);padding:12px;border-radius:8px;}";
        html += ".status-label{font-size:12px;color:#aaa;}.status-value{font-size:18px;font-weight:bold;}";
        html += ".online{color:#2ecc71;}.offline{color:#e74c3c;}";
        html += "button{padding:6px 12px;border:none;border-radius:5px;cursor:pointer;transition:0.2s;}";
        html += "button.primary{background:#3498db;color:white;}button.danger{background:#e74c3c;color:white;}button.warning{background:#f39c12;color:white;}";
        html += "input,select{padding:8px;border-radius:5px;border:1px solid #555;background:#2c3e50;color:white;}";
        html += ".remote-panel{display:" + String(currentWorkMode == MODE_REMOTE ? "block" : "none") + ";}";
        html += ".printer-panel{display:" + String(currentWorkMode == MODE_PRINTER ? "block" : "none") + ";}";
        html += ".keyboard{display:grid;grid-template-columns:repeat(10,1fr);gap:8px;margin-top:15px;}";
        html += ".key{background:#2c3e50;padding:12px;text-align:center;border-radius:8px;cursor:pointer;transition:0.1s;}";
        html += ".key:active{background:#e67e22;transform:scale(0.95);}.key.wide{grid-column:span 2;}";
        html += ".modifier-row{display:flex;gap:10px;margin-bottom:15px;}";
        html += ".modifier{background:#34495e;padding:10px 15px;border-radius:8px;cursor:pointer;}.modifier.active{background:#e67e22;}";
        html += ".text-input{display:flex;gap:10px;margin-bottom:15px;}.text-input input{flex:1;}";
        html += "@media(max-width:768px){.keyboard{grid-template-columns:repeat(5,1fr);}}";
        html += "</style><script>";
        html += "let ws=null;";
        html += "function connectWS(){ws=new WebSocket('ws://'+window.location.hostname+':81');ws.onopen=()=>console.log('WS ok');ws.onclose=()=>setTimeout(connectWS,3000);}";
        html += "function sendCmd(c){if(ws&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify(c));}";
        html += "function sendKey(k){sendCmd({type:'key',key:k});}function sendChar(c){sendCmd({type:'char',char:c});}";
        html += "function sendText(){let t=document.getElementById('textInput').value;if(t){sendCmd({type:'text',text:t});document.getElementById('textInput').value='';}}";
        html += "function toggleModifier(m){let b=document.getElementById('mod_'+m);let a=b.classList.toggle('active');sendCmd({type:'modifier',modifier:m,pressed:a});}";
        html += "function sendCombined(k){sendCmd({type:'combined',key:k});}function sendMedia(k){sendCmd({type:'media',media:k});}";
        html += "function switchMode(m){fetch('/api/mode?mode='+m).then(()=>setTimeout(()=>location.reload(),500));}";
        html += "function reboot(){if(confirm('Reboot?'))fetch('/api/reboot');}";
        html += "function loadWiFiList(){fetch('/api/networks').then(r=>r.json()).then(d=>{let h='</td><th>SSID</th><th>Signal</th><th>Action</th><tr>';d.forEach(n=>{if(n.ssid)h+=`<tr><td>${n.ssid}</td><td>${n.rssi}dBm</td><td><button onclick='connectWiFi(\"${n.ssid}\")'>Connect</button></td>`;});h+='</table>';document.getElementById('wifiList').innerHTML=h;});}";
        html += "function connectWiFi(ssid){let p=prompt('Password for '+ssid+':');if(p){fetch('/api/wifi/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:p})}).then(()=>{alert('Added, rebooting...');setTimeout(()=>location.reload(),3000);});}}";
        html += "function addWiFi(){let s=document.getElementById('newSsid').value;let p=document.getElementById('newPwd').value;if(s){fetch('/api/wifi/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p})}).then(()=>location.reload());}}";
        html += "function clearWiFi(){if(confirm('Clear all?'))fetch('/api/wifi/clear').then(()=>location.reload());}";
        html += "setInterval(()=>{fetch('/api/status').then(r=>r.json()).then(d=>{document.getElementById('mode_display').innerText=d.mode==='printer'?'🖨️ Printer':'🎮 Remote';document.getElementById('device_status').innerHTML=d.printer_ready?'🖨️ Printer Ready':(d.usb_hid_ready||d.ble_ready)?'🎮 Remote Ready':'❌ Not Ready';document.getElementById('ip_display').innerText=d.ip;document.getElementById('wifi_display').innerText=d.ssid;document.getElementById('heap_display').innerText=d.heap;});},3000);";
        html += "window.onload=()=>{connectWS();loadWiFiList();};</script></head><body><div class='container'>";
        html += "<h1>🔌 ESP32-S3 双模式桥接器 v5.2</h1>";
        html += "<div class='mode-switch'><button class='mode-btn printer" + String(currentWorkMode == MODE_PRINTER ? " active" : "") + "' onclick='switchMode(\"printer\")'>🖨️ 打印模式</button>";
        html += "<button class='mode-btn remote" + String(currentWorkMode == MODE_REMOTE ? " active" : "") + "' onclick='switchMode(\"remote\")'>🎮 远控模式</button></div>";
        html += "<div class='card'><h3>📊 系统状态</h3><div class='status-grid'>";
        html += "<div class='status-item'><div class='status-label'>工作模式</div><div class='status-value' id='mode_display'>-</div></div>";
        html += "<div class='status-item'><div class='status-label'>设备状态</div><div class='status-value' id='device_status'>-</div></div>";
        html += "<div class='status-item'><div class='status-label'>IP地址</div><div class='status-value' id='ip_display'>-</div></div>";
        html += "<div class='status-item'><div class='status-label'>WiFi</div><div class='status-value' id='wifi_display'>-</div></div>";
        html += "<div class='status-item'><div class='status-label'>空闲内存</div><div class='status-value' id='heap_display'>-</div></div>";
        html += "</div></div>";
        html += "<div class='printer-panel'><div class='card'><h3>🖨️ 打印模式</h3><p>RAW 端口：<strong>" + String(PRINT_PORT) + "</strong></p><p>Windows 添加打印机：Standard TCP/IP Port → ESP32 IP → 端口 " + String(PRINT_PORT) + "</p></div></div>";
        html += "<div class='remote-panel'><div class='card'><h3>🎮 远控模式</h3>";
        html += "<div class='modifier-row'><div class='modifier' id='mod_ctrl' onclick='toggleModifier(\"ctrl\")'>Ctrl</div><div class='modifier' id='mod_alt' onclick='toggleModifier(\"alt\")'>Alt</div><div class='modifier' id='mod_shift' onclick='toggleModifier(\"shift\")'>Shift</div><div class='modifier' id='mod_gui' onclick='toggleModifier(\"gui\")'>Win</div></div>";
        html += "<div class='text-input'><input type='text' id='textInput' placeholder='输入文本后点击发送...'><button class='primary' onclick='sendText()'>📤 发送</button></div><div class='keyboard'>";
        for(int i=1;i<=10;i++) html += "<div class='key' onclick='sendChar(\"" + String(i%10) + "\")'>" + String(i%10) + "</div>";
        const char* keys[] = {"q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m"};
        for(auto& k : keys) html += "<div class='key' onclick='sendChar(\"" + String(k) + "\")'>" + String(k) + "</div>";
        const char* funcs[] = {"space","enter","tab","esc","backspace","del","up","down","left","right"};
        for(auto& f : funcs) html += "<div class='key wide' onclick='sendKey(\"" + String(f) + "\")'>" + String(f) + "</div>";
        html += "</div><div style='margin-top:15px;'><h4>🎵 媒体控制</h4><div class='keyboard'>";
        const char* media[] = {"volup","voldown","mute","playpause","next","prev"};
        for(auto& m : media) html += "<div class='key' onclick='sendMedia(\"" + String(m) + "\")'>" + String(m) + "</div>";
        html += "</div></div></div></div>";
        html += "<div class='card'><h3>📶 WiFi 管理</h3><div id='wifiList'>加载中...</div><hr><div style='display:flex;gap:10px;margin-top:10px;'>";
        html += "<input type='text' id='newSsid' placeholder='SSID'><input type='password' id='newPwd' placeholder='密码'>";
        html += "<button class='primary' onclick='addWiFi()'>添加</button><button class='danger' onclick='clearWiFi()'>清空全部</button></div></div>";
        html += "<div class='card'><button class='danger' onclick='reboot()'>🔄 重启设备</button></div></div></body></html>";
        server.send(200, "text/html", html);
    });
    
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
    
    server.on("/api/mode", HTTP_GET, []() {
        if (server.hasArg("mode")) {
            String mode = server.arg("mode");
            if (mode == "printer") switchWorkMode(MODE_PRINTER);
            else if (mode == "remote") switchWorkMode(MODE_REMOTE);
            server.send(200, "application/json", "{\"status\":\"ok\"}");
        } else server.send(400, "application/json", "{\"status\":\"error\"}");
    });
    
    server.on("/api/networks", HTTP_GET, []() {
        int n = WiFi.scanComplete();
        StaticJsonDocument<4096> doc; 
        JsonArray arr = doc.to<JsonArray>();
        
        if (n == -1) { 
            WiFi.scanNetworks(true);  // 启动异步扫描
            server.send(202, "application/json", "{\"status\":\"scanning\"}"); 
            return; 
        }
        
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
        WiFi.scanDelete();  // 释放扫描资源
    });
    
    server.on("/api/wifi/add", HTTP_POST, []() {
        if (server.hasArg("plain")) {
            StaticJsonDocument<256> doc;
            if (!deserializeJson(doc, server.arg("plain"))) {
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
    
    server.on("/api/wifi/clear", HTTP_GET, []() { 
        clearAllWiFi(); 
        server.send(200, "application/json", "{\"status\":\"ok\"}"); 
    });
    
    server.on("/api/reboot", HTTP_GET, []() { 
        server.send(200, "application/json", "{\"status\":\"rebooting\"}"); 
        delay(100); 
        ESP.restart(); 
    });
    
    server.begin();
    Serial.println("[Web] Web server started");
}

// ====================== 主程序 ======================
void setup() {
    Serial.begin(115200); 
    delay(1000);
    Serial.println("\n=== ESP32-S3 Dual Mode USB Bridge v5.2 ===\n");
    
    prefs.begin("usb_bridge", false); 
    loadWiFiConfigs();
    
    currentWorkMode = (prefs.getInt("work_mode", 1) == 0) ? MODE_PRINTER : MODE_REMOTE;
    
    if (currentWorkMode == MODE_REMOTE) {
        initRemoteMode();
    } else {
        initUsbHost();
    }
    
    connectToBestWiFi();
    setupWebServer();
    webSocket.begin(); 
    webSocket.onEvent(onWebSocketEvent);
    printServer.begin();
    
    Serial.printf("[INIT] Ready! IP: %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
    if (currentNetMode == MODE_AP) {
        dnsServer.processNextRequest();
    }
    
    server.handleClient(); 
    webSocket.loop();
    
    if (currentWorkMode == MODE_PRINTER && printerReady) {
        WiFiClient client = printServer.available();
        if (client && client.connected()) {
            while (client.connected()) {
                int len = client.available();
                if (len > 0) {
                    len = min(len, USB_RECV_BUF_SIZE);
                    int bytesRead = client.read(recvBuf, len);
                    if (bytesRead > 0 && !sendToPrinter(recvBuf, bytesRead)) {
                        break;
                    }
                }
                delay(1);
            }
            client.stop();
        }
    }
    
    delay(10);
}
