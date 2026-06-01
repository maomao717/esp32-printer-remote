/**
 * ESP32-S3 双模式 USB 桥接器 v4.0 - 支持有线+无线远控
 * 远控模式：USB HID（有线）+ BLE Keyboard（蓝牙）双支持
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "USB.h"
#include "USBHID.h"
#include "usb_host.h"

// ====================== 引入 TinyUSB HID 库（用于有线键盘）======================
#include <Adafruit_TinyUSB.h>

// ====================== 配置常量 ======================
const char* AP_SSID         = "ESP32-Bridge";
const char* AP_PASSWORD     = "12345678";
const int   PRINT_PORT      = 9100;
const int   WEB_PORT        = 80;
const int   WS_PORT         = 81;
const int   MAX_WIFI_NETWORKS = 5;

// 工作模式
enum WorkMode { MODE_PRINTER, MODE_REMOTE };
enum NetMode { MODE_WIFI_CLIENT, MODE_AP };
enum UsbDeviceType { DEV_NONE, DEV_PRINTER, DEV_KEYBOARD, DEV_MOUSE, DEV_UNKNOWN };

// ====================== 全局变量 ======================
WorkMode currentWorkMode = MODE_REMOTE;
NetMode currentNetMode = MODE_WIFI_CLIENT;
UsbDeviceType connectedDevice = DEV_NONE;

bool printerReady = false;
bool remoteReady = false;

// ===== 有线 HID 相关 =====
Adafruit_USBD_HID usb_hid;
bool usbHidReady = false;

// ===== 蓝牙 HID 相关 =====
#include <BleKeyboard.h>
BleKeyboard bleKeyboard("ESP32 Remote", "Espressif", 100);
bool bleReady = false;

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

// 打印机相关
WiFiServer printServer(PRINT_PORT);
static usb_host_client_handle_t clientHandle = NULL;
static usb_device_handle_t      deviceHandle = NULL;
static uint8_t                  bulkOutEpAddr = 0;
static uint8_t                  bulkOutEpMps  = 64;

// 当前按下的修饰键
bool ctrlPressed = false, altPressed = false, shiftPressed = false, guiPressed = false;

#define USB_RECV_BUF_SIZE 4096
static uint8_t recvBuf[USB_RECV_BUF_SIZE];

// ====================== 有线 HID 键盘报告函数 ======================
void sendUsbKeyReport(uint8_t modifiers, uint8_t keycode) {
    if (!usbHidReady) return;
    uint8_t report[8] = {0};
    report[0] = modifiers;
    report[2] = keycode;
    usb_hid.sendReport(1, report, sizeof(report));
}

void sendUsbKeyPress(uint8_t keycode) {
    sendUsbKeyReport(0, keycode);
    delay(20);
    sendUsbKeyReport(0, 0);
}

void sendUsbKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
    uint8_t modifiers = 0;
    if (ctrl)  modifiers |= 0x01;
    if (shift) modifiers |= 0x02;
    if (alt)   modifiers |= 0x04;
    if (gui)   modifiers |= 0x08;
    sendUsbKeyReport(modifiers, keycode);
    delay(50);
    sendUsbKeyReport(0, 0);
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

// ====================== 统一的键盘发送接口 ======================
void sendRemoteKeyPress(uint8_t keycode) {
    if (usbHidReady) {
        sendUsbKeyPress(keycode);
    } else if (bleReady && bleKeyboard.isConnected()) {
        bleKeyboard.write(keycode);
    }
}

void sendRemoteKeyPressWithModifiers(uint8_t keycode, bool ctrl, bool alt, bool shift, bool gui) {
    if (usbHidReady) {
        sendUsbKeyPressWithModifiers(keycode, ctrl, alt, shift, gui);
    } else if (bleReady && bleKeyboard.isConnected()) {
        uint8_t modifiers = 0;
        if (ctrl) modifiers |= 0x01;
        if (shift) modifiers |= 0x02;
        if (alt) modifiers |= 0x04;
        if (gui) modifiers |= 0x08;
        bleKeyboard.sendReport((KeyReport){modifiers, 0, keycode, 0, 0, 0});
        delay(50);
        bleKeyboard.sendReport((KeyReport){0, 0, 0, 0, 0, 0});
    }
}

void sendRemoteString(const char* text) {
    if (usbHidReady) {
        sendUsbString(text);
    } else if (bleReady && bleKeyboard.isConnected()) {
        bleKeyboard.print(text);
    }
}

// ====================== USB 设备识别 ======================
void identifyUsbDevice(const usb_device_desc_t *devDesc) {
    if ((devDesc->idVendor == 0x04B8 || devDesc->idVendor == 0x03F0 || 
         devDesc->idVendor == 0x04A9 || devDesc->idVendor == 0x0E6A) ||
        (devDesc->bDeviceClass == 0x07)) {
        connectedDevice = DEV_PRINTER;
        Serial.println("[USB] 识别为：打印机");
    } else {
        connectedDevice = DEV_UNKNOWN;
    }
}

static void usbEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg) {
    if (eventMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        Serial.println("[USB] 检测到新设备...");
        esp_err_t err = usb_host_device_open(clientHandle, eventMsg->new_dev.address, &deviceHandle);
        if (err != ESP_OK) return;
        const usb_device_desc_t *devDesc;
        usb_host_get_device_descriptor(deviceHandle, &devDesc);
        identifyUsbDevice(devDesc);
        if (currentWorkMode == MODE_PRINTER && connectedDevice == DEV_PRINTER) {
            const usb_config_desc_t *cfgDesc;
            usb_host_get_active_config_descriptor(deviceHandle, &cfgDesc);
            int offset = 0;
            while (offset < cfgDesc->wTotalLength) {
                const usb_standard_desc_t *desc = (const usb_standard_desc_t *)((uint8_t*)cfgDesc + offset);
                if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                    const usb_ep_desc_t *ep = (const usb_ep_desc_t*)desc;
                    bool isBulk = (ep->bmAttributes & 0x03) == USB_TRANSFER_TYPE_BULK;
                    bool isOut  = (ep->bEndpointAddress & 0x80) == 0;
                    if (isBulk && isOut) {
                        bulkOutEpAddr = ep->bEndpointAddress;
                        bulkOutEpMps  = ep->wMaxPacketSize;
                    }
                }
                offset += desc->bLength;
            }
            if (bulkOutEpAddr != 0) {
                usb_host_interface_claim(clientHandle, deviceHandle, 0, 0);
                printerReady = true;
                Serial.println("[USB] 打印机已就绪！");
            }
        }
    } else if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        printerReady = false;
        connectedDevice = DEV_NONE;
        if (deviceHandle) {
            usb_host_interface_release(clientHandle, deviceHandle, 0);
            usb_host_device_close(clientHandle, deviceHandle);
            deviceHandle = NULL;
        }
        bulkOutEpAddr = 0;
    }
}

static void sendToPrinter(const uint8_t *data, size_t len) {
    if (!printerReady || deviceHandle == NULL || bulkOutEpAddr == 0) return;
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = min((size_t)(64 * 8), len - sent);
        usb_transfer_t *transfer = NULL;
        if (usb_host_transfer_alloc(chunk, 0, &transfer) == ESP_OK) {
            memcpy(transfer->data_buffer, data + sent, chunk);
            transfer->num_bytes = chunk;
            transfer->device_handle = deviceHandle;
            transfer->bEndpointAddress = bulkOutEpAddr;
            transfer->callback = [](usb_transfer_t *t) {};
            usb_host_transfer_submit(transfer);
            sent += chunk;
        } else break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ====================== WebSocket 命令处理 ======================
void handleRemoteCommand(JsonObject& cmd) {
    String type = cmd["type"];
    if (type == "key") {
        String keyName = cmd["key"];
        if (keyName == "enter") sendRemoteKeyPress(HID_KEY_ENTER);
        else if (keyName == "space") sendRemoteKeyPress(HID_KEY_SPACE);
        else if (keyName == "backspace") sendRemoteKeyPress(HID_KEY_BACKSPACE);
        else if (keyName == "up") sendRemoteKeyPress(HID_KEY_ARROW_UP);
        else if (keyName == "down") sendRemoteKeyPress(HID_KEY_ARROW_DOWN);
        else if (keyName == "left") sendRemoteKeyPress(HID_KEY_ARROW_LEFT);
        else if (keyName == "right") sendRemoteKeyPress(HID_KEY_ARROW_RIGHT);
    } else if (type == "char") {
        String ch = cmd["char"];
        if (ch.length() == 1) {
            char c = ch[0];
            if (c >= 'a' && c <= 'z') sendRemoteKeyPress(HID_KEY_A + (c - 'a'));
            else if (c >= 'A' && c <= 'Z') sendRemoteKeyPressWithModifiers(HID_KEY_A + (c - 'A'), false, false, true, false);
            else if (c >= '0' && c <= '9') sendRemoteKeyPress(HID_KEY_1 + (c - '1'));
            else if (c == ' ') sendRemoteKeyPress(HID_KEY_SPACE);
        }
    } else if (type == "text") {
        String text = cmd["text"];
        sendRemoteString(text.c_str());
    } else if (type == "combined") {
        String keyName = cmd["key"];
        uint8_t keyCode = 0;
        if (keyName == "c") keyCode = HID_KEY_C;
        else if (keyName == "v") keyCode = HID_KEY_V;
        else if (keyName == "x") keyCode = HID_KEY_X;
        else if (keyName == "a") keyCode = HID_KEY_A;
        else if (keyName == "z") keyCode = HID_KEY_Z;
        if (keyCode != 0) {
            sendRemoteKeyPressWithModifiers(keyCode, ctrlPressed, altPressed, shiftPressed, guiPressed);
        }
    } else if (type == "modifier") {
        String mod = cmd["modifier"];
        bool pressed = cmd["pressed"];
        if (mod == "ctrl") ctrlPressed = pressed;
        else if (mod == "alt") altPressed = pressed;
        else if (mod == "shift") shiftPressed = pressed;
        else if (mod == "gui") guiPressed = pressed;
    }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            Serial.printf("[WS] 客户端 %u 已连接\n", num);
            break;
        case WStype_DISCONNECTED:
            Serial.printf("[WS] 客户端 %u 已断开\n", num);
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
    for (int i = 0; i < savedNetworkCount; i++) {
        if (savedNetworks[i].ssid == ssid) {
            savedNetworks[i].password = password;
            saveWiFiConfigs();
            return;
        }
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

void startAPMode() {
    currentNetMode = MODE_AP;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("[NET] 热点模式，IP: %s\n", WiFi.softAPIP().toString());
}

bool tryConnectWiFi(int index) {
    if (index >= savedNetworkCount) return false;
    String ssid = savedNetworks[index].ssid;
    String pwd = savedNetworks[index].password;
    Serial.printf("[NET] 尝试连接: %s\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pwd.c_str());
    unsigned long start = millis();
    while (millis() - start < 10000) {
        if (WiFi.status() == WL_CONNECTED) {
            currentNetMode = MODE_WIFI_CLIENT;
            Serial.printf("[NET] 连接成功，IP: %s\n", WiFi.localIP().toString());
            return true;
        }
        delay(200);
    }
    return false;
}

void connectToBestWiFi() {
    if (savedNetworkCount == 0) { startAPMode(); return; }
    for (int i = 0; i < savedNetworkCount; i++) {
        if (tryConnectWiFi(i)) return;
    }
    startAPMode();
}

// ====================== 网页服务器 ======================
void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'>";
        html += "<title>ESP32 双模式桥接器</title><style>";
        html += "body{font-family:Arial;background:#1a1a2e;color:#eee;padding:20px;}";
        html += ".container{max-width:800px;margin:0 auto;}h1{color:#00d4ff;}";
        html += ".card{background:rgba(255,255,255,0.1);border-radius:10px;padding:20px;margin-bottom:20px;}";
        html += "button{padding:8px 16px;margin:5px;border:none;border-radius:5px;cursor:pointer;}";
        html += ".primary{background:#3498db;color:white;}.danger{background:#e74c3c;color:white;}";
        html += ".keyboard{display:grid;grid-template-columns:repeat(10,1fr);gap:8px;margin-top:15px;}";
        html += ".key{background:#2c3e50;padding:10px;text-align:center;border-radius:5px;cursor:pointer;}";
        html += ".modifier{background:#34495e;padding:8px 12px;border-radius:5px;cursor:pointer;display:inline-block;margin:5px;}";
        html += ".modifier.active{background:#e67e22;}</style>";
        html += "<script>let ws=null;function connectWS(){ws=new WebSocket('ws://'+location.hostname+':81');ws.onopen=()=>console.log('WS ok');ws.onclose=()=>setTimeout(connectWS,3000);}";
        html += "function sendCmd(cmd){if(ws&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify(cmd));}";
        html += "function sendKey(k){sendCmd({type:'key',key:k});}function sendChar(c){sendCmd({type:'char',char:c});}";
        html += "function sendText(){let t=document.getElementById('txt').value;if(t){sendCmd({type:'text',text:t});document.getElementById('txt').value='';}}";
        html += "function toggleMod(m){let btn=document.getElementById('mod_'+m);let active=btn.classList.toggle('active');sendCmd({type:'modifier',modifier:m,pressed:active});}";
        html += "function sendCombined(k){sendCmd({type:'combined',key:k});}function switchMode(m){fetch('/api/mode?mode='+m).then(()=>location.reload());}";
        html += "window.onload=connectWS;</script></head><body><div class='container'>";
        html += "<h1>🔌 ESP32-S3 双模式桥接器</h1>";
        html += "<div style='display:flex;gap:20px;margin-bottom:20px;'>";
        html += "<button class='primary' onclick='switchMode(\"printer\")'>🖨️ 打印模式</button>";
        html += "<button class='primary' onclick='switchMode(\"remote\")'>🎮 远控模式</button></div>";
        html += "<div class='card' id='remotePanel' style='display:" + String(currentWorkMode == MODE_REMOTE ? "block" : "none") + "'>";
        html += "<h3>🎮 远程控制</h3><div><div class='modifier' id='mod_ctrl' onclick='toggleMod(\"ctrl\")'>Ctrl</div>";
        html += "<div class='modifier' id='mod_alt' onclick='toggleMod(\"alt\")'>Alt</div>";
        html += "<div class='modifier' id='mod_shift' onclick='toggleMod(\"shift\")'>Shift</div>";
        html += "<div class='modifier' id='mod_gui' onclick='toggleMod(\"gui\")'>Win</div></div>";
        html += "<div style='margin:15px 0;display:flex;gap:10px;'><input id='txt' placeholder='输入文本...' style='flex:1;padding:8px;'>";
        html += "<button class='primary' onclick='sendText()'>发送</button></div>";
        html += "<div class='keyboard'>";
        const char* keys[] = {"q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m"};
        for(auto& k : keys) html += "<div class='key' onclick='sendChar(\"" + String(k) + "\")'>" + String(k) + "</div>";
        const char* funcs[] = {"space","enter","backspace","up","down","left","right"};
        for(auto& f : funcs) html += "<div class='key' onclick='sendKey(\"" + String(f) + "\")'>" + String(f) + "</div>";
        html += "</div><div style='margin-top:15px;'>";
        html += "<div class='key' onclick='sendCombined(\"c\")'>Ctrl+C</div>";
        html += "<div class='key' onclick='sendCombined(\"v\")'>Ctrl+V</div>";
        html += "<div class='key' onclick='sendCombined(\"x\")'>Ctrl+X</div>";
        html += "<div class='key' onclick='sendCombined(\"a\")'>Ctrl+A</div></div>";
        html += "<p>💡 " + String(usbHidReady ? "USB有线已连接" : (bleReady ? "蓝牙待配对" : "未就绪")) + "</p></div>";
        html += "<div class='card' id='printerPanel' style='display:" + String(currentWorkMode == MODE_PRINTER ? "block" : "none") + "'>";
        html += "<h3>🖨️ 打印模式</h3><p>RAW端口: <strong>9100</strong></p>";
        html += "<p>电脑添加网络打印机，地址为 ESP32 IP，端口 9100</p></div></div></body></html>";
        server.send(200, "text/html", html);
    });
    
    server.on("/api/mode", HTTP_GET, []() {
        if (server.hasArg("mode")) {
            String mode = server.arg("mode");
            currentWorkMode = (mode == "printer") ? MODE_PRINTER : MODE_REMOTE;
            prefs.putInt("work_mode", currentWorkMode);
            server.send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            server.send(400, "application/json", "{\"status\":\"error\"}");
        }
    });
    
    server.on("/api/reboot", HTTP_GET, []() {
        server.send(200, "application/json", "{\"status\":\"rebooting\"}");
        delay(100);
        ESP.restart();
    });
    
    server.begin();
    Serial.println("[WEB] HTTP 服务器已启动");
}

// ====================== 主程序 ======================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== ESP32-S3 双模式桥接器 v4.1 ===");
    
    prefs.begin("usb_bridge", false);
    loadWiFiConfigs();
    currentWorkMode = prefs.getInt("work_mode", 1) == 1 ? MODE_REMOTE : MODE_PRINTER;
    
    // 初始化 USB 有线 HID
    usb_hid.begin();
    for (int i = 0; i < 50; i++) {
        if (TinyUSBDevice.mounted()) {
            usbHidReady = true;
            Serial.println("[USB_HID] USB 有线键盘已就绪");
            break;
        }
        delay(100);
    }
    if (!usbHidReady) Serial.println("[USB_HID] USB 未连接，将使用蓝牙模式");
    
    // 初始化蓝牙
    bleKeyboard.begin();
    bleReady = true;
    Serial.println("[BLE] 蓝牙键盘已启动");
    remoteReady = usbHidReady || bleReady;
    
    // USB Host
    usb_host_config_t host_config = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    usb_host_install(&host_config);
    usb_host_client_register(&clientHandle);
    usb_host_client_register_events(clientHandle, USB_HOST_CLIENT_EVENT_FLAGS_ALL, usbEventCallback, NULL);
    USB.begin();
    
    // 网络
    connectToBestWiFi();
    setupWebServer();
    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    printServer.begin();
    
    Serial.printf("[INIT] 完成\n");
}

void loop() {
    if (currentNetMode == MODE_AP) dnsServer.processNextRequest();
    server.handleClient();
    webSocket.loop();
    
    if (currentWorkMode == MODE_PRINTER && printerReady) {
        usb_host_client_handle_events(clientHandle, 0);
        WiFiClient client = printServer.available();
        if (client && client.connected()) {
            while (client.connected()) {
                int len = client.available();
                if (len > 0) {
                    len = min(len, USB_RECV_BUF_SIZE);
                    client.read(recvBuf, len);
                    sendToPrinter(recvBuf, len);
                }
                delay(1);
            }
            client.stop();
        }
    } else {
        delay(10);
    }
}
