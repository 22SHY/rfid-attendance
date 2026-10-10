/*
  ESP8266 + MFRC522 RFID attendance -> Google Sheets (Apps Script), with DS3231 clock

  Wiring (NodeMCU):
    MFRC522 : D2=SDA, D5=SCK, D7=MOSI, D6=MISO, D1=RST, 3V3, GND
    DS3231  : D3=SDA, D4=SCL, 3V3, GND      (NOT 5V)
    Buzzer  : D0 -> buzzer (+), GND -> buzzer (-)

  Beeps:  1 short  = logged (or duplicate skipped)
          2 quick  = card unknown / inactive
          3 long   = WiFi or server error

  Every scan is sent with the clock's time (&ts=). If the clock is not set or
  lost power, the ts is left out and Google uses its own time instead.

  Serial Monitor command to set the clock (local shop time):
    set 2026-10-10 15:20:00
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include "config.h"

#define SS_PIN   D2
#define RST_PIN  D1
#define BUZZER   D0
#define SDA_PIN  D3
#define SCL_PIN  D4     // note: this is also the onboard LED pin, so the LED is not used
#define DS3231_ADDR 0x68

const unsigned long SAME_CARD_DELAY_MS = 3000;  // ignore the same card tapped again within 3 s

MFRC522 rfid(SS_PIN, RST_PIN);
String lastUid = "";
unsigned long lastScanMs = 0;

// ---------------------------------------------------------------- buzzer
void beep(int onMs, int times = 1) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(onMs);
    digitalWrite(BUZZER, LOW);
    if (i < times - 1) delay(100);
  }
}

// ---------------------------------------------------------------- DS3231 clock
static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool readTime(int &y, int &mo, int &d, int &h, int &mi, int &s) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(DS3231_ADDR, 7) != 7) return false;
  s  = bcd2dec(Wire.read() & 0x7F);
  mi = bcd2dec(Wire.read() & 0x7F);
  h  = bcd2dec(Wire.read() & 0x3F);   // 24-hour mode
  Wire.read();                        // day of week (unused)
  d  = bcd2dec(Wire.read() & 0x3F);
  mo = bcd2dec(Wire.read() & 0x1F);
  y  = 2000 + bcd2dec(Wire.read());
  return true;
}

// Oscillator Stop Flag: 1 = clock lost power / never set
bool timeNotTrusted() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  if (Wire.endTransmission() != 0) return true;
  if (Wire.requestFrom(DS3231_ADDR, 1) != 1) return true;
  return (Wire.read() & 0x80) != 0;
}

bool setTime(int y, int mo, int d, int h, int mi, int s) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(s));
  Wire.write(dec2bcd(mi));
  Wire.write(dec2bcd(h));
  Wire.write(1);                      // day of week, not used
  Wire.write(dec2bcd(d));
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd(y - 2000));
  if (Wire.endTransmission() != 0) return false;

  Wire.beginTransmission(DS3231_ADDR);   // clear Oscillator Stop Flag
  Wire.write(0x0F);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_ADDR, 1);
  uint8_t st = Wire.read();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  Wire.write(st & 0x7F);
  return Wire.endTransmission() == 0;
}

// Returns "YYYY-MM-DD HH:MM:SS", or "" if the clock cannot be trusted
String clockTimestamp() {
  int y, mo, d, h, mi, s;
  if (!readTime(y, mo, d, h, mi, s)) return "";
  if (timeNotTrusted() || y < 2024) return "";
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
  return String(buf);
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  int y, mo, d, h, mi, s;
  if (line.startsWith("set ") &&
      sscanf(line.c_str() + 4, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
    Serial.println(setTime(y, mo, d, h, mi, s) ? "Clock set." : "Set FAILED (I2C error)");
    Serial.println("Clock now: " + clockTimestamp());
  } else {
    Serial.println("Unknown command. Use:  set YYYY-MM-DD HH:MM:SS");
  }
}

// ---------------------------------------------------------------- WiFi
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nConnected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi failed");
  }
}

// Make text safe to put inside a URL (spaces -> %20 etc.)
String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[((unsigned char)c >> 4) & 0xF];
      out += hex[(unsigned char)c & 0xF];
    }
  }
  return out;
}

String readUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// Step 1: call the script (it runs now, Google answers with a redirect).
// Step 2: follow the redirect on a fresh connection to read the reply.
// ran = true means the script executed, even if the reply could not be read.
String fetchWithRedirect(const String& url, int& finalCode, bool& ran) {
  ran = false;
  finalCode = 0;
  String location;
  {
    WiFiClientSecure c1;
    c1.setInsecure();
    HTTPClient h1;
    if (!h1.begin(c1, url)) return "";
    const char* keys[] = {"Location"};
    h1.collectHeaders(keys, 1);
    h1.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    h1.setTimeout(15000);
    int code = h1.GET();
    finalCode = code;
    if (code == 200) {
      ran = true;
      return h1.getString();
    }
    if (code == 301 || code == 302 || code == 303 || code == 307) {
      ran = true;
      location = h1.header("Location");
    }
    h1.end();
  }                                   // first connection is freed here
  if (location.length() == 0) return "";

  WiFiClientSecure c2;
  c2.setInsecure();
  HTTPClient h2;
  if (!h2.begin(c2, location)) return "";
  h2.setTimeout(15000);
  finalCode = h2.GET();
  String body = (finalCode == 200) ? h2.getString() : "";
  h2.end();
  return body;
}

void sendScan(const String& uid, const String& ts) {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi");
    beep(600, 3);
    return;
  }

  String url = String(SCRIPT_URL) + "?uid=" + uid + "&dev=" + urlEncode(DEVICE_NAME);
  if (ts.length() > 0) url += "&ts=" + urlEncode(ts);
  if (strlen(SECRET_KEY) > 0) url += "&key=" + urlEncode(SECRET_KEY);

  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  int code;
  bool ran;
  String payload = fetchWithRedirect(url, code, ran);
  payload.trim();
  Serial.printf("HTTP %d, delivered=%d -> %s\n", code, ran, payload.c_str());

  if (payload.startsWith("OK")) {
    beep(150);                                          // logged
  } else if (payload.indexOf("CARD_UNKNOWN") >= 0 || payload.indexOf("CARD_INACTIVE") >= 0) {
    beep(100, 2);                                       // card problem
  } else if (payload.length() > 0) {
    beep(600, 3);                                       // script returned some other error
  } else if (ran) {
    beep(150);                                          // reached Google, reply not read
  } else {
    beep(600, 3);                                       // never reached Google
  }
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  SPI.begin();
  rfid.PCD_Init();

  String now = clockTimestamp();
  if (now.length() > 0) Serial.println("Clock: " + now);
  else Serial.println("Clock NOT SET or not found - set it with:  set YYYY-MM-DD HH:MM:SS");

  connectWiFi();
  beep(100, 2);
  Serial.println("Ready. Tap a card.");
}

void loop() {
  handleSerialCommands();
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = readUid();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == lastUid && millis() - lastScanMs < SAME_CARD_DELAY_MS) return;
  lastUid = uid;
  lastScanMs = millis();

  String ts = clockTimestamp();            // time of the tap, taken right now
  Serial.println("Card: " + uid + "  time: " + (ts.length() ? ts : String("(no clock)")));
  sendScan(uid, ts);
}