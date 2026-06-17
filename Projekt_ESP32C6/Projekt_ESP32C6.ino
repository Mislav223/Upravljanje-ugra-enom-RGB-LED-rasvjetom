/*
 * Projekt: Ambient Light senzor s RGB LED-om, OLED ekranom i BLE
 * Mikrokontroler: ESP32-C6-DevKitC-1
 *
 * Opis rada:
 *   - BH1750 senzor mjeri jakost svjetla (lux)
 *   - SSD1306 OLED prikazuje ocitanje i status sistema
 *   - WS2812 LED mijenja intenzitet i boju ovisno o ambijentalnom svjetlu
 *   - BLE prima upravljačke naredbe (promjena boje/režima rada)
 */

#include <Arduino.h>
#include <U8g2lib.h>      
#include <Wire.h>         
#include <BH1750.h>       
#include <Adafruit_NeoPixel.h>  
#include <NimBLEDevice.h> 

// ── Hardverske konfiguracije i pinovi ────────────────────────────────
#define I2C_SDA     6
#define I2C_SCL     7
#define LED_PIN     8       // Pin za kontrolu WS2812 LED-ice
#define LED_COUNT   1       
#define BH1750_ADDR 0x23    // I2C adresa svjetlosnog senzora

// ── BLE UUID-ovi (Nordic UART Service standard) ──────────────────────
#define BLE_DEVICE_NAME   "ESP32-LightSensor"
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // RX (prijem)
#define NUS_TX_CHAR_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // TX (slanje)

// ── Kalibracijski i radni parametri ──────────────────────────────────
#define CALIB_SAMPLES   10      // Broj uzoraka za određivanje baseline-a
#define CALIB_SCALE     4.0f    // Gornja granica skale (baseline * 4)
#define CALIB_MIN_LUX   20.0f   // Minimalna dopuštena vrijednost za baseline
#define BRIGHT_MAX      255     
#define BRIGHT_MIN      5       // Minimalni intenzitet u automatskom modu

// ── Struktura i tablica boja ──────────────────────────────────────────
struct Color { uint8_t r, g, b; const char* name; };

const Color COLORS[] = {
    { 255,   0,   0, "RED"     },
    {   0, 255,   0, "GREEN"   },
    {   0,   0, 255, "BLUE"    },
    { 255, 255, 255, "WHITE"   },
    { 255, 200,   0, "YELLOW"  },
    {   0, 255, 220, "CYAN"    },
    { 180,   0, 255, "MAGENTA" },
    {   0,   0,   0, "OFF"     },
};
const int COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

// ── Globalne varijable i instance objekata ────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
BH1750            lightMeter;
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

float   luxBaseline  = 0.0f;  
float   luxMax       = 0.0f;  
int     colorIndex   = 0;     
bool    autoMode     = true;  // true = automatska svjetlina, false = ručni mod
bool    bleConnected = false;

// Asinkroni prihvat BLE naredbi (pohrana za izvršavanje u glavnoj petlji)
bool    cmdPending   = false;
String  pendingCmd   = "";

NimBLECharacteristic* pTxChar = nullptr;  


// ── BLE Callback Klase ────────────────────────────────────────────────

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        bleConnected = true;
        Serial.println("[BLE] Uređaj spojen");
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        bleConnected = false;
        Serial.println("[BLE] Uređaj odspojen - pokretanje oglašavanja");
        NimBLEDevice::startAdvertising();  
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        std::string raw = pChar->getValue();  
        if (raw.empty()) return;

        String cmd = String(raw.c_str());
        cmd.trim();         
        cmd.toUpperCase();  

        // Zapisivanje naredbe u međuspremnik radi thread-safetyja
        pendingCmd = cmd;
        cmdPending = true;
        Serial.printf("[BLE] Primljena naredba: \"%s\"\n", cmd.c_str());
    }
};


// ── Pomoćne funkcije ──────────────────────────────────────────────────

// Izračun intenziteta LED-a: veća količina ambijentalnog svjetla smanjuje sjaj LED-ice
uint8_t luxToBrightness(float lux) {
    float clamped  = constrain(lux, 0.0f, luxMax);
    float inverted = 1.0f - (clamped / luxMax); // Inverzija skale
    return (uint8_t)(BRIGHT_MIN + inverted * (BRIGHT_MAX - BRIGHT_MIN));
}

// Primjena odabrane boje i skaliranje intenziteta svjetline
void applyLED(uint8_t brightness) {
    const Color& c = COLORS[colorIndex];
    uint8_t r = (uint8_t)((c.r * brightness) / 255);
    uint8_t g = (uint8_t)((c.g * brightness) / 255);
    uint8_t b = (uint8_t)((c.b * brightness) / 255);
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();  
}

// Slanje povratne informacije klijentu putem BLE Notifikacije
void bleNotify(const char* msg) {
    if (pTxChar && bleConnected) {
        pTxChar->setValue(msg);
        pTxChar->notify();  
    }
}

// Inicijalna kalibracija senzora u trenutnom prostoru (uzimanje prosjeka uzoraka)
float calibrate() {
    Serial.println("[CAL] Uzimanje uzoraka ambijentalnog svjetla...");
    float sum   = 0.0f;
    int   valid = 0;

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        float lux = lightMeter.readLightLevel();
        if (lux >= 0) { sum += lux; valid++; }  

        int barWidth = map(i + 1, 0, CALIB_SAMPLES, 0, 126);

        display.clearBuffer();
        display.setFont(u8g2_font_6x10_tr);
        display.drawStr(0, 10, "Kalibracija...");
        display.drawHLine(0, 13, 128);
        display.drawFrame(0, 28, 128, 10);  
        display.drawBox(1, 29, barWidth, 8); 
        display.sendBuffer();
        delay(200);
    }

    float baseline = (valid > 0) ? (sum / valid) : CALIB_MIN_LUX;
    baseline = max(baseline, CALIB_MIN_LUX);
    Serial.printf("[CAL] Baseline: %.2f lx | Max gornja granica: %.2f lx\n",
                  baseline, baseline * CALIB_SCALE);
    return baseline;
}

// Obrada primljenih BLE naredbi
void processCommand(const String& cmd) {
    if (cmd == "AUTO") {
        autoMode = true;
        bleNotify("MODE:AUTO");
        Serial.println("[CMD] Automatski mod uključen");
        return;
    }
    if (cmd == "MANUAL") {
        autoMode = false;
        bleNotify("MODE:MANUAL");
        Serial.println("[CMD] Ručni mod uključen (fiksni intenzitet)");
        return;
    }

    // Pretraživanje tablice boja
    for (int i = 0; i < COLOR_COUNT; i++) {
        if (cmd == COLORS[i].name) {
            colorIndex = i;
            char reply[32];
            snprintf(reply, sizeof(reply), "COLOR:%s", COLORS[i].name);
            bleNotify(reply);
            Serial.printf("[CMD] Promjena boje -> %s\n", COLORS[i].name);
            return;
        }
    }

    bleNotify("ERR:UNKNOWN");
    Serial.printf("[CMD] Nepoznata naredba: \"%s\"\n", cmd.c_str());
}


// ── BLE Inicijalizacija ───────────────────────────────────────────────
void setupBLE() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);

    // Definiranje RX karakteristike (pisanje od strane klijenta)
    NimBLECharacteristic* pRxChar = pService->createCharacteristic(
        NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxChar->setCallbacks(new RxCallbacks());

    // Definiranje TX karakteristike (slanje notifikacija klijentu)
    pTxChar = pService->createCharacteristic(
        NUS_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    pService->start();

    // Pokretanje oglašavanja (Advertising)
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(NUS_SERVICE_UUID);
    NimBLEDevice::startAdvertising();

    Serial.println("[OK] BLE podsustav aktivan");
}


// ── Vizualizacija na OLED ekranu ─────────────────────────────────────
void drawDisplay(float lux, uint8_t brightness) {
    display.clearBuffer();

    // Ispis zaglavlja i BLE statusne ikone
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(0, 10, "Ambient Light");
    if (bleConnected)
        display.drawDisc(123, 5, 4);    
    else
        display.drawCircle(123, 5, 4);  
    display.drawHLine(0, 13, 128);

    // Prikaz trenutnog očitavanja u luksima
    display.setFont(u8g2_font_ncenB24_tr);
    char luxBuf[10];
    sprintf(luxBuf, lux < 10000.0f ? "%.1f" : "%.0f", lux);
    display.drawStr(0, 44, luxBuf);

    // Prikaz statusnih informacija na dnu ekrana
    display.setFont(u8g2_font_6x10_tr);
    char ceilBuf[12];
    sprintf(ceilBuf, "/%4.0flx", luxMax);
    display.drawStr(0, 54, ceilBuf);

    display.drawStr(50, 54, COLORS[colorIndex].name);
    display.drawStr(100, 54, autoMode ? "AUTO" : "MAN");

    // Traka za prikaz trenutnog intenziteta LED-a (grafički indikator)
    int barWidth = map(brightness, BRIGHT_MIN, BRIGHT_MAX, 0, 126);
    display.drawFrame(0, 56, 128, 8);
    display.drawBox(1, 57, barWidth, 6);

    display.sendBuffer();  
}


// ── Setup funkcija ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA, I2C_SCL);  

    // Inicijalizacija BH1750 senzora
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_ADDR, &Wire)) {
        Serial.println("[ERROR] Senzor BH1750 nije detektiran na I2C sabirnici!");
        while (true) delay(1000);  
    }
    Serial.println("[OK] BH1750 senzor inicijaliziran");

    display.begin();
    Serial.println("[OK] SSD1306 OLED inicijaliziran");

    led.begin();
    led.clear();
    led.show();  
    Serial.println("[OK] WS2812 LED modul inicijaliziran");

    // Pokretanje procesa kalibracije okoline
    luxBaseline = calibrate();
    luxMax      = luxBaseline * CALIB_SCALE;

    setupBLE();
}


// ── Glavna petlja programa (Loop) ─────────────────────────────────────
void loop() {
    // Obrada asinkrono primljenih BLE naredbi
    if (cmdPending) {
        cmdPending = false;
        processCommand(pendingCmd);
    }

    // Čitanje vrijednosti osvjetljenja
    float lux = lightMeter.readLightLevel();
    if (lux < 0) {
        Serial.println("[WARN] Greška prilikom čitanja podataka s senzora");
        delay(500);
        return;  
    }

    // Određivanje intenziteta rada LED modula ovisno o režimu rada
    uint8_t brightness = autoMode ? luxToBrightness(lux) : BRIGHT_MAX;

    applyLED(brightness);
    drawDisplay(lux, brightness);

    // Ispis telemetrije na Serial sučelje
    Serial.printf("Lux: %8.2f | Boja: %-7s | Mod: %-6s | LED Intenzitet: %3d/255\n",
                  lux,
                  COLORS[colorIndex].name,
                  autoMode ? "AUTO" : "MANUAL",
                  brightness);

    delay(500);  
}