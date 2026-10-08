/*
  ESP8266 + MFRC522 RFID attendance -> Google Sheets (Apps Script)

  Wiring (NodeMCU -> MFRC522): D2=SDA, D5=SCK, D7=MOSI, D6=MISO, D1=RST, 3V3, GND
  Buzzer: D0 -> buzzer (+), GND -> buzzer (-)

  Beeps:  1 short  = logged (or duplicate skipped)
          2 quick  = card unknown / inactive
          3 long   = WiFi or server error
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include "config.h"

#define SS_PIN   D2
#define RST_PIN  D1
#define BUZZER   D0
#define LED      LED_BUILTIN   // active LOW on NodeMCU

const unsigned long SAME_CARD_DELAY_MS = 3000;  // ignore the same card tapped again within 3 s

MFRC522 rfid(SS_PIN, RST_PIN);
String lastUid = "";
unsigned long lastScanMs = 0;

void beep(int onMs, int times = 1) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    digitalWrite(LED, LOW);
    delay(onMs);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED, HIGH);
    if (i < times - 1) delay(100);
  }
}

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

void sendScan(const String& uid) {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi");
    beep(600, 3);
    return;
  }

  String url = String(SCRIPT_URL) + "?uid=" + uid + "&dev=" + urlEncode(DEVICE_NAME);
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

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  pinMode(LED, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(LED, HIGH);

  SPI.begin();
  rfid.PCD_Init();
  connectWiFi();
  beep(100, 2);
  Serial.println("Ready. Tap a card.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = readUid();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == lastUid && millis() - lastScanMs < SAME_CARD_DELAY_MS) return;
  lastUid = uid;
  lastScanMs = millis();

  Serial.println("Card: " + uid);
  sendScan(uid);
}