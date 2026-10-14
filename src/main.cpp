/*
  ESP8266 + MFRC522 RFID attendance -> Google Sheets (Apps Script)
  with DS3231 clock and an offline queue (saved on the board, uploaded later)

  Wiring (NodeMCU):
    MFRC522 : D2=SDA, D5=SCK, D7=MOSI, D6=MISO, D1=RST, 3V3, GND
    DS3231  : D3=SDA, D4=SCL, 3V3, GND      (NOT 5V)
    Green LED: D0 -> 330 ohm resistor -> LED long leg (+); LED short leg (-) -> GND
    Red LED  : D8 -> 330 ohm resistor -> LED long leg (+); LED short leg (-) -> GND

  How a tap is handled (outbox pattern):
    1. Take the time from the DS3231.
    2. Save "UID,time" at the END of the queue file (survives power cuts).
    3. If WiFi is up, upload the queue oldest-first (up to 3 records now).
    4. Anything not confirmed stays in the queue; a background job retries it.
    The Apps Script ignores a record it already has (same UID + time), so a
    retry after a lost reply can never create a duplicate row.

  LED signals (no buzzer in this version):
    Power on / WiFi connected   = light show: green, red, then both
    Idle (ready to tap)         = RED stays on, steady
    Card accepted (in or out)   = GREEN on for 5 seconds, red off
    Card unknown / inactive     = red blinks 3 times
    No WiFi (saved on board)    = green and red blink alternately
    Error (scan not saved)      = green and red both on, long light

  Serial Monitor commands:
    set 2026-10-10 15:20:00     set the clock (local shop time)
    queue                       show how many scans are waiting
    clearqueue                  delete waiting scans (testing only!)
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <Wire.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include "config.h"

#define SS_PIN   D2
#define RST_PIN  D1
#define LED_GREEN D0
#define LED_RED   D8     // D8 must stay LOW at boot: the LED + resistor to GND does that
#define GREEN_HOLD_MS 5000   // how long green stays on after an accepted card
#define ERROR_HOLD_MS 3000   // how long both LEDs stay on for an error
#define SDA_PIN  D3
#define SCL_PIN  D4     // also the onboard LED pin, so the LED is not used
#define DS3231_ADDR 0x68

const unsigned long SAME_CARD_DELAY_MS = 3000;  // ignore the same card tapped again within 3 s
const char* QUEUE_FILE = "/queue.txt";
const char* QUEUE_TMP  = "/queue.tmp";
const int   MAX_QUEUE = 500;          // max waiting scans (about 16 KB)
const int   MAX_VERIFY_TRIES = 5;     // reached Google but reply unreadable this many times -> stop retrying

MFRC522 rfid(SS_PIN, RST_PIN);
String lastUid = "";
unsigned long lastScanMs = 0;
bool fsOk = false;
int headTries = 0;
unsigned long lastNetFailMs = 0;      // when Google was last unreachable (0 = fine)

// ---------------------------------------------------------------- LEDs
enum Signal { SIG_OK, SIG_CARD_PROBLEM, SIG_SAVED, SIG_ERROR };

unsigned long successUntilMs = 0;     // green stays on until this time (0 = not holding)
bool wifiWasUp = false;

void setLeds(bool green, bool red) {
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
  digitalWrite(LED_RED, red ? HIGH : LOW);
}

// Light show: green, red, then both
void ledSelfTest() {
  setLeds(true, false);  delay(250);
  setLeds(false, true);  delay(250);
  setLeds(true, true);   delay(250);
  setLeds(false, false);
}

// Result of a tap. Idle is "red steady"; every signal ends back in that state.
void showSignal(Signal sig) {
  successUntilMs = 0;                                   // a new result cancels any green hold
  switch (sig) {
    case SIG_OK:                                        // accepted: green for 5 s, red off (not blocking)
      successUntilMs = millis() + GREEN_HOLD_MS;
      setLeds(true, false);
      return;
    case SIG_CARD_PROBLEM:                              // red blinks 3 times
      for (int i = 0; i < 3; i++) {
        setLeds(false, false); delay(300);
        setLeds(false, true);  delay(300);
      }
      break;
    case SIG_SAVED:                                     // no WiFi: green and red blink alternately
      for (int i = 0; i < 3; i++) {
        setLeds(true, false);  delay(300);
        setLeds(false, true);  delay(300);
      }
      break;
    case SIG_ERROR:                                     // error: both on, long light
      setLeds(true, true);
      delay(ERROR_HOLD_MS);
      break;
  }
  setLeds(false, true);                                 // back to idle: red steady
}

// Called every pass of loop(): keeps the idle / green-hold state and repeats the light show when WiFi comes back
void ledTick() {
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up && !wifiWasUp) ledSelfTest();                  // WiFi just connected
  wifiWasUp = up;
  if (millis() < successUntilMs) setLeds(true, false);  // holding green
  else setLeds(false, true);                            // idle: red steady
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

// ---------------------------------------------------------------- WiFi
void connectWiFi() {            // blocking, used once at startup
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
    Serial.println("\nWiFi failed (scans will be saved and uploaded later)");
  }
}

// Non-blocking: taps keep working while WiFi is down
void keepWiFiAlive() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < 20000) return;
  lastTry = millis();
  Serial.println("WiFi down - trying to reconnect");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
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

// ---------------------------------------------------------------- Google call
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

String sendRecord(const String& uid, const String& ts, int& code, bool& ran) {
  String url = String(SCRIPT_URL) + "?uid=" + uid + "&dev=" + urlEncode(DEVICE_NAME);
  if (ts.length() > 0) url += "&ts=" + urlEncode(ts);
  if (strlen(SECRET_KEY) > 0) url += "&key=" + urlEncode(SECRET_KEY);
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  String payload = fetchWithRedirect(url, code, ran);
  payload.trim();
  Serial.printf("HTTP %d, delivered=%d -> %s\n", code, ran, payload.c_str());
  return payload;
}

void signalForReply(const String& payload, bool ran) {
  if (payload.startsWith("OK")) {
    showSignal(SIG_OK);                                 // CHECK-IN, CHECK-OUT or an already-logged duplicate
  } else if (payload.indexOf("CARD_UNKNOWN") >= 0 || payload.indexOf("CARD_INACTIVE") >= 0) {
    showSignal(SIG_CARD_PROBLEM);
  } else if (payload.length() > 0) {
    showSignal(SIG_ERROR);                              // script returned some other error
  } else if (ran) {
    showSignal(SIG_OK);                                 // reached Google, reply not read (no-clock path only)
  } else {
    showSignal(SIG_ERROR);                              // never reached Google
  }
}

// ---------------------------------------------------------------- queue (LittleFS)
bool popQueue();

int queueCount() {
  if (!fsOk || !LittleFS.exists(QUEUE_FILE)) return 0;
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    if (f.read() == '\n') n++;
  }
  f.close();
  return n;
}

bool enqueue(const String& uid, const String& ts) {
  if (!fsOk) return false;
  if (queueCount() >= MAX_QUEUE) return false;
  File f = LittleFS.open(QUEUE_FILE, "a");
  if (!f) return false;
  size_t w = f.print(uid + "," + ts + "\n");
  f.close();
  return w > 0;
}

// Reads the OLDEST waiting scan. Returns false if the queue is empty.
bool peekQueue(String& uid, String& ts) {
  if (!fsOk || !LittleFS.exists(QUEUE_FILE)) return false;
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return false;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  int comma = line.indexOf(',');
  if (comma < 1) {                    // damaged line: drop it and look at the next one
    Serial.println("Dropping damaged queue line");
    popQueue();
    return peekQueue(uid, ts);
  }
  uid = line.substring(0, comma);
  ts  = line.substring(comma + 1);
  return true;
}

// Removes the OLDEST scan. Writes a temp file first so a power cut cannot lose the rest.
bool popQueue() {
  if (!fsOk || !LittleFS.exists(QUEUE_FILE)) return false;
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return false;
  String all = f.readString();
  f.close();
  int nl = all.indexOf('\n');
  String rest = (nl < 0) ? "" : all.substring(nl + 1);
  if (rest.length() == 0) return LittleFS.remove(QUEUE_FILE);
  File t = LittleFS.open(QUEUE_TMP, "w");
  if (!t) return false;
  t.print(rest);
  t.close();
  LittleFS.remove(QUEUE_FILE);
  return LittleFS.rename(QUEUE_TMP, QUEUE_FILE);
}

// Replies after which the scan is finished and can leave the queue
bool isFinalReply(const String& r) {
  return r.startsWith("OK") ||
         r.indexOf("CARD_UNKNOWN") >= 0 || r.indexOf("CARD_INACTIVE") >= 0 ||
         r.indexOf("BAD_TIMESTAMP") >= 0 || r.indexOf("MISSING_UID") >= 0;
}

bool netRecentlyFailed() {
  return lastNetFailMs != 0 && millis() - lastNetFailMs < 60000;
}

// Uploads waiting scans oldest-first. Returns how many left the queue.
int flushQueue(int maxRecords, String& lastReply, bool& lastRan) {
  int removed = 0;
  for (int n = 0; n < maxRecords; n++) {
    String uid, ts;
    if (!peekQueue(uid, ts)) break;                     // queue empty
    int code;
    bool ran;
    String reply = sendRecord(uid, ts, code, ran);
    lastReply = reply;
    lastRan = ran;
    lastNetFailMs = ran ? 0 : millis();

    bool done = isFinalReply(reply);
    if (!done && reply.length() == 0 && ran) {          // reached Google, reply unreadable
      if (++headTries >= MAX_VERIFY_TRIES) {
        Serial.println("Reply never readable; scan did reach Google - removing from queue");
        done = true;
      }
    }
    if (!done) break;                                   // keep it and retry later
    popQueue();
    headTries = 0;
    removed++;
  }
  return removed;
}

// Called from loop(): slowly empties the queue when nothing else is going on
void backgroundFlush() {
  static unsigned long lastTry = 0;
  static unsigned long interval = 5000;
  if (millis() - lastTry < interval) return;
  lastTry = millis();
  if (WiFi.status() != WL_CONNECTED || !fsOk) { interval = 5000; return; }
  if (millis() - lastScanMs < 5000) { interval = 2000; return; }   // just after a tap
  if (queueCount() == 0) { interval = 5000; return; }
  String reply;
  bool ran = false;
  int sent = flushQueue(1, reply, ran);
  interval = (sent > 0) ? 500 : 30000;
}

// ---------------------------------------------------------------- a card tap
void handleTap(const String& uid) {
  String ts = clockTimestamp();
  Serial.println("Card: " + uid + "  time: " + (ts.length() ? ts : String("(no clock)")));

  // No trusted clock or no storage: a scan cannot be saved for later
  if (ts.length() == 0 || !fsOk) {
    if (WiFi.status() == WL_CONNECTED) {
      int code;
      bool ran;
      String reply = sendRecord(uid, ts, code, ran);
      signalForReply(reply, ran);
    } else {
      Serial.println("No WiFi and cannot save: scan LOST");
      showSignal(SIG_ERROR);
    }
    return;
  }

  if (!enqueue(uid, ts)) {
    Serial.println("Cannot save scan (storage full?)");
    showSignal(SIG_ERROR);
    return;
  }
  Serial.printf("Saved. %d waiting\n", queueCount());

  String reply = "";
  bool ran = false;
  if (WiFi.status() == WL_CONNECTED && !netRecentlyFailed()) flushQueue(3, reply, ran);

  if (queueCount() == 0) signalForReply(reply, ran);    // this tap was handled
  else showSignal(SIG_SAVED);                               // saved, uploads when online
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
  } else if (line == "queue") {
    Serial.printf("Queue: %d waiting\n", queueCount());
  } else if (line == "clearqueue") {
    if (fsOk) {
      LittleFS.remove(QUEUE_FILE);
      LittleFS.remove(QUEUE_TMP);
    }
    Serial.println("Queue cleared.");
  } else {
    Serial.println("Commands:  set YYYY-MM-DD HH:MM:SS  |  queue  |  clearqueue");
  }
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  ledSelfTest();

  Wire.begin(SDA_PIN, SCL_PIN);
  SPI.begin();
  rfid.PCD_Init();

  fsOk = LittleFS.begin();
  if (!fsOk) {
    Serial.println("LittleFS failed - offline saving disabled");
  } else if (!LittleFS.exists(QUEUE_FILE) && LittleFS.exists(QUEUE_TMP)) {
    LittleFS.rename(QUEUE_TMP, QUEUE_FILE);             // recover from a power cut mid-update
  }
  Serial.printf("Queue: %d waiting\n", queueCount());

  String now = clockTimestamp();
  if (now.length() > 0) Serial.println("Clock: " + now);
  else Serial.println("Clock NOT SET or not found - set it with:  set YYYY-MM-DD HH:MM:SS");

  connectWiFi();
  wifiWasUp = (WiFi.status() == WL_CONNECTED);
  if (wifiWasUp) ledSelfTest();                         // same light show once WiFi is connected
  setLeds(false, true);                                 // idle: red steady
  Serial.println("Ready. Tap a card.");
}

void loop() {
  handleSerialCommands();
  keepWiFiAlive();
  backgroundFlush();
  ledTick();

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = readUid();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == lastUid && millis() - lastScanMs < SAME_CARD_DELAY_MS) return;
  lastUid = uid;
  lastScanMs = millis();

  handleTap(uid);
}