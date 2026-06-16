/*
 * Projekt: Ambient Light senzor s RGB LED-om, OLED ekranom i BLE
 * Mikrokontroler: ESP32-C6-DevKitC-1
 *
 * Sto radi:
 *   - BH1750 senzor mjeri jakost svjetla (lux)
 *   - SSD1306 OLED prikazuje ocitanje i status
 *   - WS2812 LED mijenja boju i brightness ovisno o svjetlu
 *   - BLE prima naredbe (npr. boja ili rezim rada) preko nRF Connect apka
 *
 * Spojevi:
 *   SDA -> GPIO6 | SCL -> GPIO7 | VCC -> 3.3V | GND -> GND
 */

// Preduvjeti za pokretanje:
// (0) https://www.wch-ic.com/download/file?id=331 (CH343 driver, da laptop zna prepoznat USB kad se spoji da je ESP32)
// (1) 3. gumb lijevo (biblioteke), postavis Type: "Installed" (na racunalu)
// (2) File > Preferences > Additional Boards Manager URL: https://espressif.github.io/arduino-esp32/package_esp32_index.json
// (3) Kod šalješ na pločicu (flashas) tako sto (GORE) odaberes port (COM#) (Win+R -> devmgmt.msc), zatim STRELICU KOJA POKAZUJE DESNO (Upload)
// (4) Dok radi, možeš promatrati ispis na Serial Monitoru (Tools > Serial Monitor) koji radi na 115200 baud

#include <Arduino.h>
#include <U8g2lib.h>      // biblioteka za OLED ekran
#include <Wire.h>         // I2C komunikacija
#include <BH1750.h>       // lux senzor
#include <Adafruit_NeoPixel.h>  // WS2812 LED kontrola
#include <NimBLEDevice.h> // BLE (Bluetooth Low Energy)

// ── pinovi i hardverske konstante ────────────────────────────────────
#define I2C_SDA     6
#define I2C_SCL     7
#define LED_PIN     8       // pin na koji je spojen WS2812
#define LED_COUNT   1       // koristimo samo jednu LED
#define BH1750_ADDR 0x23    // I2C adresa senzora (zadana adresa)

// ── BLE UUIDs - Nordic UART Service (standardni UUID-ovi za BLE uart) ─
#define BLE_DEVICE_NAME   "ESP32-LightSensor"
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // telefon pise ovdje
#define NUS_TX_CHAR_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // ESP salje notifikacije

// ── parametri za kalibraciju ──────────────────────────────────────────
#define CALIB_SAMPLES   10      // broj uzoraka pri pokretanju
#define CALIB_SCALE     4.0f    // maksimalni lux = baseline * 4
#define CALIB_MIN_LUX   20.0f   // minimalna vrijednost baseline-a (zastita od mraka)
#define BRIGHT_MAX      255     // maksimalni brightness LED-a
#define BRIGHT_MIN      5       // minimalni brightness (ne zelimo potpuni off u auto modu)

// ── tablica boja ──────────────────────────────────────────────────────
// struct grupira R, G, B i ime boje u jedan objekt
// ?? profesori cesto pitaju: zasto struct umjesto tri zasebna arraya?
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
// ?? sizeof trik: ukupna velicina niza / velicina jednog elementa = broj elemenata
const int COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

// ── globalne varijable ────────────────────────────────────────────────
// U8G2 objekt za OLED (HW I2C, 128x64 piksela)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
BH1750            lightMeter;
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

float   luxBaseline  = 0.0f;  // prosjecni lux izmjeren pri pokretanju
float   luxMax       = 0.0f;  // gornja granica za brightness racun
int     colorIndex   = 0;     // indeks trenutne boje u COLORS[]
bool    autoMode     = true;  // true = brightness ovisi o luxu, false = fiksni 255
bool    bleConnected = false;
// ?? zasto pendingCmd umjesto direktnog poziva u callbacku?
//    BLE callback se izvrsava u drugom tasku, pa je sigurnije samo sacuvati naredbu
//    i obraditi je u glavnoj petlji (loop)
bool    cmdPending   = false;
String  pendingCmd   = "";

NimBLECharacteristic* pTxChar = nullptr;  // koristimo za slanje odgovora prema telefonu


// ── BLE callback klase ────────────────────────────────────────────────

// ?? nasljedujemo NimBLEServerCallbacks i prepisujemo (override) metode
//    koje zelimo koristiti - tipican OOP pristup u C++
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        bleConnected = true;
        Serial.println("[BLE] Uredaj spojen");
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        bleConnected = false;
        Serial.println("[BLE] Uredaj odspojen - ponovo oglasavam");
        NimBLEDevice::startAdvertising();  // ponovo pocni oglasavanje
    }
};

// Ovaj callback se poziva kad telefon napise naredbu na RX karakteristiku
class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        std::string raw = pChar->getValue();  // citamo sto je telefon poslao
        if (raw.empty()) return;

        String cmd = String(raw.c_str());
        cmd.trim();         // micemo razmake/newline s pocetka i kraja
        cmd.toUpperCase();  // normaliziramo na velika slova

        // ?? ne pozivamo processCommand() odmah jer smo u BLE tasku
        //    samo zapisujemo naredbu i flag, loop() ce je obraditi
        pendingCmd = cmd;
        cmdPending = true;
        Serial.printf("[BLE] Primljena naredba: \"%s\"\n", cmd.c_str());
    }
};


// ── pomocne funkcije ──────────────────────────────────────────────────

// Racuna brightness LED-a na temelju izmjerenog luxa
// ?? logika: vise svjetla u sobi = manje potreban jak LED = manji brightness
uint8_t luxToBrightness(float lux) {
    // constrain drzi vrijednost unutar [0, luxMax]
    float clamped  = constrain(lux, 0.0f, luxMax);
    // normaliziramo na [0,1] i invertiramo - da bi vise luxa davalo manji brightness
    float inverted = 1.0f - (clamped / luxMax);
    // skaliramo na raspon [BRIGHT_MIN, BRIGHT_MAX]
    return (uint8_t)(BRIGHT_MIN + inverted * (BRIGHT_MAX - BRIGHT_MIN));
}

// Postavlja RGB LED na trenutnu boju, skaliranu na zadani brightness
// ?? dijeljenjem s 255 dobivamo proporcionalnu vrijednost kanala
//    npr. brightness=128 (pola) -> svaki kanal se prepolovi
void applyLED(uint8_t brightness) {
    const Color& c = COLORS[colorIndex];
    uint8_t r = (uint8_t)((c.r * brightness) / 255);
    uint8_t g = (uint8_t)((c.g * brightness) / 255);
    uint8_t b = (uint8_t)((c.b * brightness) / 255);
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();  // primijeni promjenu (bez show() LED se ne mijenja)
}

// Salje kratku poruku nazad na BLE klijent (telefon)
void bleNotify(const char* msg) {
    if (pTxChar && bleConnected) {
        pTxChar->setValue(msg);
        pTxChar->notify();  // notifikacija - telefon mora biti pretplacen
    }
}

// Kalibracija: uzima N uzoraka, racuna prosjek, prikazuje progress bar na OLED-u
// ?? prosjek se radi da se eliminira sitni sum senzora
float calibrate() {
    Serial.println("[CAL] Uzimam uzorke...");
    float sum   = 0.0f;
    int   valid = 0;

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        float lux = lightMeter.readLightLevel();
        if (lux >= 0) { sum += lux; valid++; }  // ignoriramo negativna (pogresna) ocitanja

        // ?? map() skalira vrijednost iz jednog raspona u drugi
        //    ovdje: i+1 iz [0, CALIB_SAMPLES] -> sirина bара u [0, 126] piksela
        int barWidth = map(i + 1, 0, CALIB_SAMPLES, 0, 126);

        display.clearBuffer();
        display.setFont(u8g2_font_6x10_tr);
        display.drawStr(0, 10, "Kalibracija...");
        display.drawHLine(0, 13, 128);
        display.drawFrame(0, 28, 128, 10);  // obrub progress bara
        display.drawBox(1, 29, barWidth, 8); // ispunjeni dio progress bara
        display.sendBuffer();
        delay(200);
    }

    // ako nema valjanih uzoraka, koristimo CALIB_MIN_LUX kao zastitu
    float baseline = (valid > 0) ? (sum / valid) : CALIB_MIN_LUX;
    baseline = max(baseline, CALIB_MIN_LUX);
    Serial.printf("[CAL] Baseline: %.2f lx  Strop: %.2f lx\n",
                  baseline, baseline * CALIB_SCALE);
    return baseline;
}

// Obradjuje BLE naredbe - mijenja mod ili boju
void processCommand(const String& cmd) {

    // provjera mod naredbi
    if (cmd == "AUTO") {
        autoMode = true;
        bleNotify("MODE:AUTO");
        Serial.println("[CMD] Auto brightness ukljucen");
        return;
    }
    if (cmd == "MANUAL") {
        autoMode = false;
        bleNotify("MODE:MANUAL");
        Serial.println("[CMD] Rucni brightness (fiksno 255)");
        return;
    }

    // provjera naredbi za boju - prolazimo kroz cijelu tablicu
    for (int i = 0; i < COLOR_COUNT; i++) {
        if (cmd == COLORS[i].name) {
            colorIndex = i;
            char reply[32];
            snprintf(reply, sizeof(reply), "COLOR:%s", COLORS[i].name);
            bleNotify(reply);
            Serial.printf("[CMD] Boja -> %s\n", COLORS[i].name);
            return;
        }
    }

    // ako nismo nasli naredbu, javljamo gresku
    bleNotify("ERR:UNKNOWN");
    Serial.printf("[CMD] Nepoznata naredba: \"%s\"\n", cmd.c_str());
}


// ── BLE inicijalizacija ───────────────────────────────────────────────
void setupBLE() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // maksimalna snaga odašiljanja

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);

    // RX: klijent pise, ESP cita - prima naredbe
    NimBLECharacteristic* pRxChar = pService->createCharacteristic(
        NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxChar->setCallbacks(new RxCallbacks());

    // TX: ESP salje, klijent prima - odgovori i notifikacije
    pTxChar = pService->createCharacteristic(
        NUS_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    pService->start();

    // BLE oglasavanje - da telefon moze pronaci uredaj
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(NUS_SERVICE_UUID);
    NimBLEDevice::startAdvertising();

    Serial.println("[OK] BLE aktivan: \"" BLE_DEVICE_NAME "\"");
}


// ── crtanje na OLED ekranu ────────────────────────────────────────────
void drawDisplay(float lux, uint8_t brightness) {
    display.clearBuffer();

    // naslov i BLE status (pun krug = spojen, prazan = ceka spajanje)
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(0, 10, "Ambient Light");
    if (bleConnected)
        display.drawDisc(123, 5, 4);    // popunjeni krug
    else
        display.drawCircle(123, 5, 4);  // samo obrub
    display.drawHLine(0, 13, 128);

    // veliki broj - trenutni lux
    display.setFont(u8g2_font_ncenB24_tr);
    char luxBuf[10];
    // za manje vrijednosti prikazujemo decimalу, za vece samo cijeli broj
    sprintf(luxBuf, lux < 10000.0f ? "%.1f" : "%.0f", lux);
    display.drawStr(0, 44, luxBuf);

    // donji red: strop / boja / mod
    display.setFont(u8g2_font_6x10_tr);

    char ceilBuf[12];
    sprintf(ceilBuf, "/%4.0flx", luxMax);
    display.drawStr(0, 54, ceilBuf);

    display.drawStr(50, 54, COLORS[colorIndex].name);
    display.drawStr(100, 54, autoMode ? "AUTO" : "MAN");

    // brightness traka na dnu ekrana
    // ?? map() ovdje skalira brightness (5-255) na siрinu bara (0-126 piksela)
    int barWidth = map(brightness, BRIGHT_MIN, BRIGHT_MAX, 0, 126);
    display.drawFrame(0, 56, 128, 8);
    display.drawBox(1, 57, barWidth, 6);

    display.sendBuffer();  // salji buffer na ekran (bez ovoga nista se ne prikazuje)
}


// ── setup - izvrsava se jednom pri pokretanju ─────────────────────────
void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA, I2C_SCL);  // inicijalizacija I2C sabirnice

    // pokusaj inicijalizacije BH1750
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_ADDR, &Wire)) {
        Serial.println("[ERROR] BH1750 nije pronadjen");
        while (true) delay(1000);  // stani ovdje ako senzor nije spojen
    }
    Serial.println("[OK] BH1750 spreman");

    display.begin();
    Serial.println("[OK] SSD1306 spreman");

    led.begin();
    led.clear();
    led.show();  // ugasi LED na pocetku
    Serial.println("[OK] WS2812 spreman");

    // kalibracija i postavljanje gornje granice
    luxBaseline = calibrate();
    luxMax      = luxBaseline * CALIB_SCALE;

    setupBLE();
}


// ── loop - glavna petlja, izvrsava se stalno ──────────────────────────
void loop() {

    // provjeri ima li cekajuce BLE naredbe i obradi je
    if (cmdPending) {
        cmdPending = false;
        processCommand(pendingCmd);
    }

    // citanje senzora
    float lux = lightMeter.readLightLevel();
    if (lux < 0) {
        Serial.println("[WARN] Greska citanja BH1750");
        delay(500);
        return;  // preskocimo ostatak petlje ako je ocitanje pogresno
    }

    // izracun brightness - auto mod koristi lux, rucni uvijek 255
    uint8_t brightness = autoMode ? luxToBrightness(lux) : BRIGHT_MAX;

    applyLED(brightness);
    drawDisplay(lux, brightness);

    // ispis stanja na Serial (korisno za debugging)
    Serial.printf("Lux: %8.2f  Boja: %-7s  Mod: %-6s  LED: %3d/255\n",
                  lux,
                  COLORS[colorIndex].name,
                  autoMode ? "AUTO" : "MANUAL",
                  brightness);

    delay(500);  // cekamo pola sekunde prije sljedeceg ocitanja
}