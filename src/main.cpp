#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>

static BLEUUID serviceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID charRX_UUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID charTX_UUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

#define TCP_PORT 5000
#define MAX_TCP_CLIENTS 3
#define BOOT_BUTTON_PIN 0

Preferences prefs;
AsyncWebServer webServer(80);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClients[MAX_TCP_CLIENTS];

NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pRemoteRX = nullptr;
NimBLERemoteCharacteristic* pRemoteTX = nullptr;

String wifi_ssid, wifi_pass, ble_mac, mdns_name;
uint32_t ble_pin = 123456;
bool bleConnected = false;
bool setupMode = false;

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pclient) override {
        Serial.println("[BLE] Csatlakozva.");
    }
    void onDisconnect(NimBLEClient* pclient) override {
        Serial.println("[BLE] Kapcsolat bontva.");
        bleConnected = false;
    }
    uint32_t onPassKeyRequest() override {
        return ble_pin;
    }
    bool onConfirmPIN(uint32_t pass_key) override {
        return true;
    }
};

void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    uint8_t header[3];
    header[0] = 0x3e;
    header[1] = length & 0xFF;
    header[2] = (length >> 8) & 0xFF;

    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (tcpClients[i] && tcpClients[i].connected()) {
            tcpClients[i].write(header, 3);
            tcpClients[i].write(pData, length);
        }
    }
}

bool connectToMeshCore() {
    if (ble_mac.isEmpty()) return false;

    NimBLEAddress addr(ble_mac.c_str(), BLE_ADDR_PUBLIC);
    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new ClientCallbacks(), false);
    }

    if (!pClient->connect(addr)) {
        NimBLEAddress randAddr(ble_mac.c_str(), BLE_ADDR_RANDOM);
        if (!pClient->connect(randAddr)) {
            return false;
        }
    }

    pClient->secureConnection();

    NimBLERemoteService* pService = pClient->getService(serviceUUID);
    if (!pService) {
        pClient->disconnect();
        return false;
    }

    pRemoteRX = pService->getCharacteristic(charRX_UUID);
    pRemoteTX = pService->getCharacteristic(charTX_UUID);

    if (!pRemoteRX || !pRemoteTX) {
        pClient->disconnect();
        return false;
    }

    if (pRemoteTX->canNotify()) {
        pRemoteTX->subscribe(true, notifyCallback);
    }

    bleConnected = true;
    Serial.println("[BLE] MeshCore hid aktiv!");
    return true;
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshCore BLE-TCP Híd</title>
<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:15px;background:#121212;color:#eee}
input,button{width:100%;padding:10px;margin:8px 0;border-radius:6px;border:1px solid #444;box-sizing:border-box}
input{background:#222;color:#fff}button{background:#0078d4;color:#fff;font-weight:bold;cursor:pointer;border:none}
.scan{background:#2e7d32}</style></head><body>
<h2>MeshCore BLE-TCP Bridge</h2>
<button class="scan" onclick="scanBLE()">Közeli MeshCore eszközök keresése</button>
<div id="list"></div>
<form action="/save" method="POST">
<label>Wi-Fi SSID:</label><input name="ssid" value="%SSID%" required>
<label>Wi-Fi Jelszó:</label><input type="password" name="pass" value="%PASS%">
<label>MeshCore BLE MAC-cím:</label><input id="mac" name="mac" value="%MAC%" placeholder="AA:BB:CC:DD:EE:FF" required>
<label>Bluetooth PIN (6 számjegy):</label><input name="pin" type="number" value="%PIN%" required>
<label>mDNS Név (.local):</label><input name="mdns" value="%MDNS%" required>
<button type="submit">Mentés és Újraindítás</button>
</form>
<script>
function scanBLE(){
  document.getElementById('list').innerHTML='Keresés (5 mp)...';
  fetch('/scan').then(r=>r.json()).then(d=>{
    let h=''; d.forEach(dev=>{
      h+=`<button type="button" onclick="document.getElementById('mac').value='${dev.mac}'">${dev.name} (${dev.mac})</button>`;
    });
    document.getElementById('list').innerHTML=h||'Nem található eszköz.';
  });
}
</script></body></html>
)rawliteral";

void setupWebRoutes() {
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = index_html;
        html.replace("%SSID%", wifi_ssid);
        html.replace("%PASS%", wifi_pass);
        html.replace("%MAC%", ble_mac);
        html.replace("%PIN%", String(ble_pin));
        html.replace("%MDNS%", mdns_name);
        request->send(200, "text/html", html);
    });

    webServer.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        pScan->setActiveScan(true);
        NimBLEScanResults results = pScan->start(5, false);
        String json = "[";
        for (int i = 0; i < results.getCount(); i++) {
            NimBLEAdvertisedDevice dev = results.getDevice(i);
            String name = dev.getName().c_str();
            if (name.startsWith("MeshCore") || dev.isAdvertisingService(serviceUUID)) {
                if (json.length() > 1) json += ",";
                json += "{\"name\":\"" + (name.isEmpty() ? "MeshCore" : name) + "\",\"mac\":\"" + String(dev.getAddress().toString().c_str()) + "\"}";
            }
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        prefs.begin("meshcore", false);
        if (request->hasParam("ssid", true)) prefs.putString("ssid", request->getParam("ssid", true)->value());
        if (request->hasParam("pass", true)) prefs.putString("pass", request->getParam("pass", true)->value());
        if (request->hasParam("mac", true))  prefs.putString("mac", request->getParam("mac", true)->value());
        if (request->hasParam("pin", true))  prefs.putUInt("pin", request->getParam("pin", true)->value().toInt());
        if (request->hasParam("mdns", true)) prefs.putString("mdns", request->getParam("mdns", true)->value());
        prefs.end();
        request->send(200, "text/plain", "Mentve! Ujrainditas...");
        delay(1000);
        ESP.restart();
    });

    webServer.begin();
}

void startSetupMode() {
    setupMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MeshCore-Bridge-Setup");
    setupWebRoutes();
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    NimBLEDevice::init("MeshCore-Bridge");
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

    prefs.begin("meshcore", true);
    wifi_ssid = prefs.getString("ssid", "");
    wifi_pass = prefs.getString("pass", "");
    ble_mac   = prefs.getString("mac", "");
    ble_pin   = prefs.getUInt("pin", 123456);
    mdns_name = prefs.getString("mdns", "meshcore-bridge");
    prefs.end();

    delay(3000);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW || wifi_ssid.isEmpty()) {
        startSetupMode();
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        delay(500);
        retries++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        startSetupMode();
        return;
    }

    if (MDNS.begin(mdns_name.c_str())) {
        MDNS.addService("meshcore", "tcp", TCP_PORT);
    }

    setupWebRoutes();
    tcpServer.begin();
}

void loop() {
    if (setupMode) return;

    if (!bleConnected) {
        static unsigned long lastAttempt = 0;
        if (millis() - lastAttempt > 5000) {
            lastAttempt = millis();
            connectToMeshCore();
        }
    }

    if (tcpServer.hasClient()) {
        bool assigned = false;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (!tcpClients[i] || !tcpClients[i].connected()) {
                tcpClients[i] = tcpServer.accept();
                assigned = true;
                break;
            }
        }
        if (!assigned) tcpServer.accept().stop();
    }

    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (tcpClients[i] && tcpClients[i].connected() && tcpClients[i].available() >= 3) {
            if (tcpClients[i].peek() == 0x3c) {
                uint8_t header[3];
                tcpClients[i].read(header, 3);
                uint16_t frameLen = header[1] | (header[2] << 8);

                if (frameLen > 0 && frameLen <= 512) {
                    uint8_t buf[516];
                    size_t readBytes = tcpClients[i].readBytes(buf, frameLen);
                    if (readBytes == frameLen && bleConnected && pRemoteRX) {
                        pRemoteRX->writeValue(buf, frameLen, false);
                    }
                }
            } else {
                tcpClients[i].read();
            }
        }
    }
}
