# SVEUČILIŠTE JOSIPA JURJA STROSSMAYERA U OSIJEKU
# FAKULTET PRIMIJENJENE MATEMATIKE I INFORMATIKE
**UGRAĐENI SUSTAVI**

## SEMINARSKI RAD
### Upravljanje ugrađenom RGB LED rasvjetom putem ESP32 mikrokontrolera, BH1750 senzora i BLE komunikacije

**Mislav Mišković**  
Osijek, 2026.

---

## Sadržaj
1. [Uvod](#uvod)
2. [Opis sustava](#opis-sustava)
3. [Tehničke specifikacije](#tehničke-specifikacije)
    - [Hardver](#hardver)
    - [Softver](#softver)
4. [Implementacija](#implementacija)
    - [Struktura programa](#struktura-programa)
    - [Kalibracija senzora](#kalibracija-senzora)
    - [Automatska regulacija svjetlosti](#automatska-regulacija-svjetlosti)
    - [BLE komunikacija](#ble-komunikacija)
    - [Prikaz na OLED zaslonu](#prikaz-na-oled-zaslonu)
5. [Tablica komponenti](#tablica-komponenti)
6. [Zaključak](#zaključak)
7. [Literatura](#literatura)

---

## Uvod
U ovom seminarskom radu opisan je sustav za automatsko upravljanje RGB LED rasvjetom koji koristi mikrokontroler **ESP32-C6** kao središnju upravljačku jedinicu. Cilj projekta bio je izraditi funkcionalan sustav koji mjeri jakost ambijentalne rasvjete pomoću digitalnog senzora **BH1750** te na temelju izmjerene vrijednosti automatski prilagođava svjetlost ugrađene adresabilne LED diode na samoj razvojnoj pločici.

Uz automatski način rada, sustav podržava i bežično upravljanje putem **Bluetooth Low Energy (BLE)** sučelja. Korisnik se može spojiti na uređaj korištenjem mobilne aplikacije **nRF Connect for Mobile** i slanjem tekstualnih naredbi mijenjati boju LED-a ili prebacivati između automatskog i ručnog načina rada.

Na OLED zaslonu (**SSD1306**) prikazuju se trenutno očitanje u lux-ima, aktivna boja, način rada te vizualni indikator svjetlosti u obliku trake. Cijeli sustav realiziran je na eksperimentalnoj pločici (*breadboard*) i programiran u Arduino okruženju koristeći **PlatformIO**.

---

## Opis sustava
Sustav se sastoji od četiri glavne komponente koje međusobno komuniciraju kako bi osigurale željenu funkcionalnost:

*   **BH1750**: Digitalni senzor ambijentalnog osvjetljenja koji komunicira s ESP32 putem **I2C** sabirnice. Konfiguriran je u `CONTINUOUS_HIGH_RES_MODE` načinu rada za precizno mjerenje od 1 do 65535 lx.
*   **ESP32-C6**: Prima podatke od senzora, izračunava odgovarajuću svjetlost za LED, obrađuje BLE naredbe te ažurira prikaz na OLED zaslonu.
*   **Ugrađena RGB LED dioda**: Adresabilna dioda (WS2812) spojena na **GPIO8**. Boja i intenzitet se dinamički prilagođavaju ili mijenjaju putem naredbi.
*   **SSD1306 OLED zaslon**: Prikazuje podatke u stvarnom vremenu (lux, boja, mod, status BLE veze).

---

## Tehničke specifikacije

### Hardver
#### ESP32-C6-DevKitC-1 s ugrađenom RGB LED diodom
ESP32-C6 se temelji na **32-bitnoj RISC-V** arhitekturi. Sadrži integrirani Wi-Fi 6 i BLE 5.0. Razvojna pločica ima ugrađenu **WS2812** LED diodu na GPIO8 pinu, što eliminira potrebu za dodatnim ožičenjem. Upravljanje se vrši pomoću *Adafruit NeoPixel* biblioteke.

#### BH1750 senzor osvjetljenja
Digitalni senzor koji koristi I2C sučelje. Adresa senzora je **0x23**. Spojen je na **GPIO6 (SDA)** i **GPIO7 (SCL)** pinove, a napajan s 3.3V.

#### SSD1306 OLED zaslon
Monokromatski zaslon rezolucije **128x64** piksela. Koristi I2C sučelje (adresa **0x3C**) i dijeli sabirnicu sa senzorom. Za upravljanje se koristi *U8g2* biblioteka.

### Softver
*   **PlatformIO i Arduino okvir**: Razvoj u VS Code-u. Korištene biblioteke: `U8g2lib`, `BH1750`, `Adafruit_NeoPixel` i `NimBLEDevice`.
*   **nRF Connect for Mobile**: Mobilna aplikacija za slanje UTF-8 tekstualnih naredbi putem *Nordic UART Servicea* (NUS).

---

## Implementacija

### Struktura programa
Program je podijeljen na: inicijalizaciju hardvera (`setup()`), kalibraciju, BLE servis, pomoćne funkcije i glavnu petlju (`loop()`) koja se izvodi svakih **500 ms**.

### Kalibracija senzora
Pri pokretanju, sustav kroz 2 sekunde uzima 10 uzoraka kako bi dobio **baseline** (referentnu vrijednost). Baseline ne smije biti manji od 20 lx, a `luxMax` se postavlja na četverostruku vrijednost baseline-a.

### Automatska regulacija svjetlosti
Funkcija `luxToBrightness()` implementira inverznu linearnu presliku:
cpp
uint8_t luxToBrightness(float lux) {
    float clamped = constrain(lux, 0.0f, luxMax);
    float inverted = 1.0f - (clamped / luxMax);
    return (uint8_t)(BRIGHT_MIN + inverted * (BRIGHT_MAX - BRIGHT_MIN));
}
Što je prostorija svjetlija, intenzitet LED-a je manji.

### BLE komunikacija
Koristi se **NUS** servis s RX (za primanje naredbi) i TX (za slanje odgovora) karakteristikama.
Podržane naredbe: `RED`, `GREEN`, `BLUE`, `WHITE`, `YELLOW`, `CYAN`, `MAGENTA`, `OFF`, `AUTO`, `MANUAL`.

### Prikaz na OLED zaslonu
Funkcija `drawDisplay()` prikazuje:
- Naziv aplikacije i BLE indikator statusa.
- Trenutnu vrijednost u lux-ima (veliki font).
- Aktivnu boju i način rada.
- "Progress bar" koji vizualizira intenzitet svjetla.

---

## Tablica komponenti

| Komponenta | Model | Napomena |
| :--- | :--- | :--- |
| Mikrokontroler i LED | ESP32-C6-DevKitC-1 | Osnovna jedinica s integriranom WS2812 LED na GPIO8. |
| Senzor svjetla | BH1750 (I2C, 0x23) | Mjeri jakost ambijentalne rasvjete u lux-ima. |
| Zaslon | SSD1306 OLED 0.96" | Prikazuje lux, boju LED-a i način rada. |
| Komunikacija | BLE (Nordic UART Service) | Naredbe putem nRF Connect aplikacije. |
| I2C pinovi | SDA: GPIO6, SCL: GPIO7 | Dijeljeni između BH1750 i OLED-a. |

---

## Zaključak
U okviru ovog seminarskog rada uspješno je realiziran sustav za automatsku regulaciju RGB LED rasvjete. Projekt je demonstrirao primjenu **I2C komunikacije** za rad s više uređaja na istoj sabirnici, upravljanje adresabilnim LED diodama te osnove **BLE servisa**. Sustav je stabilan i sve planirane funkcionalnosti su uspješno implementirane.

---

## Literatura
*   Espressif Systems, *ESP32-C6 Technical Reference Manual*, 2023.
*   ROHM Semiconductor, *BH1750FVI Datasheet*, 2011.
*   Nordic Semiconductor, *nRF Connect for Mobile dokumentacija*.
*   Adafruit Industries, *NeoPixel Überguide*.
*   U8g2 biblioteka, *oliver/U8g2_Arduino*.