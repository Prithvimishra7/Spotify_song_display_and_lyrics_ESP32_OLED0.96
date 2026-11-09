// Spotify Music Companion for ESP32
//   OLED 0.96" (SSD1306, I2C)   : album cover (B&W) + song, artist, play/pause, progress
//   Green LCD 16x2 (I2C backpack): live synced lyrics (current line + next line)
//   3 buttons                    : G25 previous, G26 play/pause, G27 next  (optional)
//
// Lyrics come from LRCLIB (free, no account needed). Not every song has synced lyrics.
//
// Wiring (OLED and LCD share the same two I2C wires):
//   OLED: GND->GND, VDD->3V3, SCK->G22, SDA->G21   (or G33/G32, the code finds it)
//   LCD : GND->GND, VCC->VIN (5V), SDA->same SDA as OLED, SCL->same SCK as OLED
//   Buttons: one leg to G25/G26/G27, diagonal leg to GND
//
// Libraries (Arduino IDE -> Library Manager):
//   - Adafruit SSD1306 (+ GFX, BusIO)       [installed]
//   - ArduinoJson by Benoit Blanchon (v7)   [installed]
//   - TJpg_Decoder by Bodmer                [installed]
//   - LiquidCrystal I2C by Frank de Brabander   [NEW]
//
// Board: ESP32 Dev Module.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TJpg_Decoder.h>
#include <LiquidCrystal_I2C.h>

// ================== FILL THESE IN ==================
const char* WIFI_SSID     = "";      // must be 2.4 GHz WiFi (or phone hotspot)
const char* WIFI_PASSWORD = "";
const char* CLIENT_ID     = "";
const char* CLIENT_SECRET = "";
const char* REFRESH_TOKEN = "";  // use the token WITH control permission for buttons
// ==================================================
// ===================================================

const int LCD_COLS = 16;
const int LCD_ROWS = 2;
// LCD I2C address: 0 = find it automatically. Or type what the I2C scanner shows, e.g. 0x27 or 0x3F.
const byte LCD_ADDRESS = 0;
// Lyrics often feel a bit late because of WiFi delay. Increase this (e.g. 500) to show lines earlier.
const long LYRIC_OFFSET_MS = 1000;
// LCD look: 1 = karaoke (words type in as they are sung, equalizer in music breaks)
//           0 = classic (current line on top, next line below)
const int LCD_STYLE = 1;
// I2C speed. 400000 = fast and smooth. If the LCD shows garbage, change to 100000.
const uint32_t I2C_CLOCK = 400000;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
LiquidCrystal_I2C* lcd = nullptr;

// ---- Spotify state ----
String accessToken = "";
unsigned long tokenExpiresAt = 0;
bool hasTrack = false;
bool isPlaying = false;
String songName = "", artistName = "";          // cleaned for the screens
String rawSong = "", rawArtist = "", rawAlbum = ""; // original text for the lyrics search
String currentTrackId = "";
long progressMs = 0;
long durationMs = 0;
unsigned long lastFetchMillis = 0;
bool fetchedOnce = false;

// ---- Album art ----
const int ART = 64;
uint8_t gray[ART * ART];
uint8_t artBitmap[ART * ART / 8];
bool hasArt = false;
String currentArtUrl = "";
String pendingArtUrl = "";
int pendingArtWidth = 64;

// ---- Lyrics ----
const int MAX_LINES = 160;
long lyricTime[MAX_LINES];
String lyricText[MAX_LINES];
int lyricCount = 0;
bool lyricsPending = false;
enum LyricState { LYR_NONE, LYR_LOADING, LYR_OK, LYR_NOT_FOUND, LYR_INSTRUMENTAL };
LyricState lyricState = LYR_NONE;
int shownLine = -2;
String lcdRow[2] = {"", ""};

// The LCD runs in its own background task (on the ESP32's other core), so lyrics keep
// moving smoothly while the main loop waits for Spotify / album art / lyrics downloads.
SemaphoreHandle_t i2cLock = nullptr;     // OLED + LCD share the I2C wires: one at a time
SemaphoreHandle_t lyricsLock = nullptr;  // protects lyrics + song text shared by both tasks

void lockI2C()      { if (i2cLock) xSemaphoreTake(i2cLock, portMAX_DELAY); }
void unlockI2C()    { if (i2cLock) xSemaphoreGive(i2cLock); }
void lockLyrics()   { if (lyricsLock) xSemaphoreTake(lyricsLock, portMAX_DELAY); }
void unlockLyrics() { if (lyricsLock) xSemaphoreGive(lyricsLock); }
unsigned long lastLcdUpdate = 0;
unsigned long lineShownAt = 0;

// ---- Buttons ----
const int buttonPins[3] = {25, 26, 27};   // previous, play/pause, next
bool lastButtonState[3] = {HIGH, HIGH, HIGH};
unsigned long lastButtonChange[3] = {0, 0, 0};
String flashText = "";
unsigned long flashUntil = 0;

// Text area on the OLED (right of the cover)
const int TEXT_X = 66;
const int TEXT_W = 128 - TEXT_X;

// ---------- helpers ----------

String cleanText(const String& in) {
  String out = "";
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c >= 32 && c <= 126) out += c;
  }
  out.trim();
  return out;
}

String urlEncode(const String& s) {
  String out = "";
  const char* hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String formatTime(long ms) {
  if (ms < 0) ms = 0;
  long totalSec = ms / 1000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld:%02ld", totalSec / 60, totalSec % 60);
  return String(buf);
}

long currentPositionMs() {
  long pos = progressMs;
  if (isPlaying) pos += (long)(millis() - lastFetchMillis);
  if (durationMs > 0 && pos > durationMs) pos = durationMs;
  return pos;
}

void oledFlush() {
  lockI2C();
  display.display();
  unlockI2C();
}

void showMessage(const char* line1, const char* line2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Spotify");
  display.drawFastHLine(0, 11, 128, SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(line1);
  display.setCursor(0, 32);
  display.println(line2);
  oledFlush();
}

// ---------- I2C devices ----------

bool setupDisplay() {
  int pinSets[2][2] = {{21, 22}, {32, 33}};
  byte addrs[2] = {0x3C, 0x3D};
  for (int p = 0; p < 2; p++) {
    Wire.begin(pinSets[p][0], pinSets[p][1]);
    for (int a = 0; a < 2; a++) {
      Wire.beginTransmission(addrs[a]);
      if (Wire.endTransmission() == 0) {
        Serial.printf("OLED found at 0x%X on SDA=G%d SCK=G%d\n", addrs[a], pinSets[p][0], pinSets[p][1]);
        Wire.setClock(I2C_CLOCK);
        display.begin(SSD1306_SWITCHCAPVCC, addrs[a]);
        return true;
      }
    }
    Wire.end();
  }
  return false;
}

// Custom LCD characters (slots 1-7, written in text as '\x01'..'\x07')
//   1-4 = equalizer bars (short -> tall)   5 = music note   6 = round dot   7 = heart
const char C_NOTE = 5, C_DOT = 6, C_HEART = 7;
void makeLcdChars() {
  uint8_t bars[4][8] = {
    {0, 0, 0, 0, 0, 0, 0x0E, 0x0E},
    {0, 0, 0, 0, 0x0E, 0x0E, 0x0E, 0x0E},
    {0, 0, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E},
    {0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E}};
  uint8_t note[8]  = {0x02, 0x03, 0x02, 0x02, 0x0E, 0x1E, 0x0C, 0x00};
  uint8_t dot[8]   = {0x00, 0x0E, 0x1F, 0x1F, 0x1F, 0x0E, 0x00, 0x00};
  uint8_t heart[8] = {0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00};
  for (int i = 0; i < 4; i++) lcd->createChar(i + 1, bars[i]);
  lcd->createChar(C_NOTE, note);
  lcd->createChar(C_DOT, dot);
  lcd->createChar(C_HEART, heart);
}

// Looks for the LCD backpack (usually 0x27 or 0x3F) on the same wires as the OLED.
// If LCD_ADDRESS is set (e.g. 0x27), that address is used directly.
void setupLCD() {
  byte found = 0;
  if (LCD_ADDRESS != 0) {
    found = LCD_ADDRESS;
  } else {
    for (byte a = 0x20; a <= 0x3F && !found; a++) {
      if (a > 0x27 && a < 0x38) continue;    // backpack addresses are 0x20-0x27 or 0x38-0x3F
      if (a == 0x3C || a == 0x3D) continue;  // these are the OLED, not the LCD
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) found = a;
    }
  }
  if (!found) {
    Serial.println("LCD not found (lyrics will be skipped). Check LCD wires / 5V.");
    return;
  }
  Serial.printf("LCD found at 0x%X\n", found);
  lcd = new LiquidCrystal_I2C(found, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  makeLcdChars();
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print("\x05Music Companion");
  lcd->setCursor(0, 1);
  lcd->print("Starting...");
}

void lcdShow(const String& r0, const String& r1) {
  if (!lcd) return;
  lockI2C();
  String rows[2] = {r0, r1};
  for (int r = 0; r < 2; r++) {
    String t = rows[r];
    if ((int)t.length() > LCD_COLS) t = t.substring(0, LCD_COLS);
    while ((int)t.length() < LCD_COLS) t += ' ';
    if (t != lcdRow[r]) {                      // only redraw what changed (I2C is slow)
      lcd->setCursor(0, r);
      lcd->print(t);
      lcdRow[r] = t;
    }
  }
  unlockI2C();
}

void connectWiFi() {
  showMessage("Connecting WiFi...", WIFI_SSID);
  lcdShow("Connecting WiFi", "");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    showMessage("WiFi connected!");
    lcdShow("WiFi connected!", "");
  } else {
    Serial.println("\nWiFi failed");
    showMessage("WiFi failed", "Check name/password");
    lcdShow("WiFi failed", "");
  }
}

// ---------- album art ----------

bool jpgToGray(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  for (int j = 0; j < h; j++) {
    int py = y + j;
    if (py < 0 || py >= ART) continue;
    for (int i = 0; i < w; i++) {
      int px = x + i;
      if (px < 0 || px >= ART) continue;
      uint16_t c = bitmap[j * w + i];
      int r = ((c >> 11) & 0x1F) * 255 / 31;
      int g = ((c >> 5) & 0x3F) * 255 / 63;
      int b = (c & 0x1F) * 255 / 31;
      gray[py * ART + px] = (r * 77 + g * 150 + b * 29) >> 8;
    }
  }
  return true;
}

void ditherArt() {
  int lo = 255, hi = 0;
  for (int i = 0; i < ART * ART; i++) {
    if (gray[i] < lo) lo = gray[i];
    if (gray[i] > hi) hi = gray[i];
  }
  int range = hi - lo;
  if (range < 1) range = 1;
  static int16_t err[ART * ART];
  for (int i = 0; i < ART * ART; i++) err[i] = (int16_t)((gray[i] - lo) * 255 / range);
  memset(artBitmap, 0, sizeof(artBitmap));
  for (int y = 0; y < ART; y++) {
    for (int x = 0; x < ART; x++) {
      int i = y * ART + x;
      int oldV = err[i];
      int newV = oldV < 128 ? 0 : 255;
      if (newV) artBitmap[y * (ART / 8) + x / 8] |= (0x80 >> (x % 8));
      int e = oldV - newV;
      if (x + 1 < ART)                err[i + 1]       += e * 7 / 16;
      if (y + 1 < ART && x > 0)       err[i + ART - 1] += e * 3 / 16;
      if (y + 1 < ART)                err[i + ART]     += e * 5 / 16;
      if (y + 1 < ART && x + 1 < ART) err[i + ART + 1] += e * 1 / 16;
    }
  }
}

bool loadAlbumArt(const String& url, int imgWidth) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return false;
  int code = https.GET();
  if (code != 200) {
    Serial.printf("Image error %d\n", code);
    https.end();
    return false;
  }
  int len = https.getSize();
  if (len <= 0 || len > 60000) {
    Serial.printf("Image size not usable: %d\n", len);
    https.end();
    return false;
  }
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { https.end(); return false; }
  WiFiClient* stream = https.getStreamPtr();
  int got = 0;
  unsigned long start = millis();
  while (got < len && millis() - start < 8000) {
    int avail = stream->available();
    if (avail > 0) got += stream->readBytes(buf + got, min(avail, len - got));
    else delay(1);
  }
  https.end();
  if (got != len) { free(buf); return false; }

  int scale = 1;
  while (imgWidth / scale > ART && scale < 8) scale *= 2;
  TJpgDec.setJpgScale(scale);
  memset(gray, 0, sizeof(gray));
  JRESULT res = TJpgDec.drawJpg(0, 0, buf, len);
  free(buf);
  if (res != JDR_OK) {
    Serial.printf("JPEG decode error %d\n", res);
    return false;
  }
  ditherArt();
  return true;
}

// ---------- lyrics (LRCLIB) ----------

// Turns "[01:23.45] some words\n[01:27.10] more words" into time + text lists.
void parseSyncedLyrics(const String& lrc) {
  lyricCount = 0;
  int start = 0;
  while (start < (int)lrc.length() && lyricCount < MAX_LINES) {
    int end = lrc.indexOf('\n', start);
    if (end < 0) end = lrc.length();
    String line = lrc.substring(start, end);
    start = end + 1;

    if (line.length() < 10 || line[0] != '[') continue;
    int close = line.indexOf(']');
    if (close < 0) continue;
    String stamp = line.substring(1, close);     // mm:ss.xx
    int colon = stamp.indexOf(':');
    if (colon < 0) continue;
    long mins = stamp.substring(0, colon).toInt();
    float secs = stamp.substring(colon + 1).toFloat();
    long t = mins * 60000L + (long)(secs * 1000);

    String text = cleanText(line.substring(close + 1));
    if (text.length() == 0) text = "...";        // music break
    lyricTime[lyricCount] = t;
    lyricText[lyricCount] = text;
    lyricCount++;
  }
}

// Tries one LRCLIB request. Returns 1 = synced lyrics, 2 = instrumental, 0 = nothing.
int requestLyrics(const String& url, bool isSearch) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return 0;
  https.addHeader("User-Agent", "ESP32-Music-Companion/1.0");
  https.setTimeout(8000);
  int code = https.GET();
  if (code != 200) {
    Serial.printf("Lyrics HTTP %d\n", code);
    https.end();
    return 0;
  }
  String body = https.getString();
  https.end();

  JsonDocument filter;
  if (isSearch) {
    filter[0]["syncedLyrics"] = true;
    filter[0]["instrumental"] = true;
  } else {
    filter["syncedLyrics"] = true;
    filter["instrumental"] = true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return 0;
  JsonVariant item;
  if (isSearch) item = doc[0];
  else item = doc.as<JsonVariant>();
  if (item.isNull()) return 0;
  if (item["instrumental"] | false) return 2;
  String synced = item["syncedLyrics"] | "";
  if (synced.length() == 0) return 0;
  lockLyrics();
  parseSyncedLyrics(synced);
  int ok = lyricCount > 0 ? 1 : 0;
  unlockLyrics();
  return ok;
}

void loadLyrics() {
  lockLyrics();
  lyricState = LYR_LOADING;
  lyricCount = 0;
  shownLine = -2;
  unlockLyrics();
  lcdShow(String(C_NOTE) + " " + songName, artistName);

  String getUrl = "https://lrclib.net/api/get?track_name=" + urlEncode(rawSong) +
                  "&artist_name=" + urlEncode(rawArtist) +
                  "&album_name=" + urlEncode(rawAlbum) +
                  "&duration=" + String(durationMs / 1000);
  int r = requestLyrics(getUrl, false);
  if (r == 0) {
    // fallback: a looser search by song + artist
    String searchUrl = "https://lrclib.net/api/search?track_name=" + urlEncode(rawSong) +
                       "&artist_name=" + urlEncode(rawArtist);
    r = requestLyrics(searchUrl, true);
  }
  lockLyrics();
  if (r == 1) lyricState = LYR_OK;
  else if (r == 2) lyricState = LYR_INSTRUMENTAL;
  else lyricState = LYR_NOT_FOUND;
  unlockLyrics();
  Serial.printf("Lyrics: %d lines (state %d)\n", lyricCount, (int)lyricState);
}

// Splits a lyric line into rows of up to 16 characters, breaking between words.
int wrapLine(const String& s, String rows[], int maxRows) {
  int n = 0;
  String cur = "";
  int i = 0;
  while (i < (int)s.length() && n < maxRows) {
    int sp = s.indexOf(' ', i);
    if (sp < 0) sp = s.length();
    String word = s.substring(i, sp);
    i = sp + 1;
    while ((int)word.length() > LCD_COLS) {          // very long word: cut it
      if (cur.length()) { rows[n++] = cur; cur = ""; if (n >= maxRows) return n; }
      rows[n++] = word.substring(0, LCD_COLS);
      word = word.substring(LCD_COLS);
      if (n >= maxRows) return n;
    }
    if (cur.length() == 0) cur = word;
    else if ((int)(cur.length() + 1 + word.length()) <= LCD_COLS) cur += " " + word;
    else { rows[n++] = cur; cur = word; }
  }
  if (cur.length() && n < maxRows) rows[n++] = cur;
  return n;
}

// Shows the lyrics:
//   short line (fits 16)  -> row 1 = current line, row 2 = next line (preview)
//   medium line (fits 32) -> the current line wrapped over both rows (no scrolling)
//   long line             -> pages of 2 rows, timed to finish before the next line
void updateLCDInner();
void lcdKaraoke(long pos, int idx);
void lcdClassic(long pos, int idx);

void updateLCD() {
  if (!lcd || !fetchedOnce || millis() - lastLcdUpdate < 50) return;
  lastLcdUpdate = millis();
  lockLyrics();
  updateLCDInner();
  unlockLyrics();
}

void lcdTask(void*) {
  for (;;) {
    updateLCD();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

String centered(const String& s) {
  int pad = (LCD_COLS - (int)s.length()) / 2;
  String out = "";
  for (int i = 0; i < pad; i++) out += ' ';
  return out + s;
}

// 16 bouncing equalizer bars. They move with the song time, so they freeze when paused.
String equalizerRow(long pos) {
  String row = "";
  for (int c = 0; c < LCD_COLS; c++) {
    float v = sinf(c * 0.8f + pos * 0.006f) + sinf(c * 1.7f - pos * 0.011f) + 0.6f * sinf(pos * 0.017f + c);
    int h = (int)((v + 2.6f) * 5.0f / 5.2f);   // 0..4
    if (h < 0) h = 0;
    if (h > 4) h = 4;
    row += (h == 0) ? ' ' : (char)h;
  }
  return row;
}

// 3 dots that disappear one by one before the next line starts (like Apple Music)
String countdownRow(long msLeft) {
  int dots = (int)((msLeft + 999) / 1000);
  if (dots > 3) dots = 3;
  if (dots < 1) dots = 1;
  String s = "";
  for (int i = 0; i < dots; i++) { if (i) s += ' '; s += C_DOT; }
  return centered(s);
}

bool isBreak(const String& s) { return s == "..." || s.length() == 0; }

void updateLCDInner() {
  if (!hasTrack) { lcdShow(centered(String(C_HEART) + " Not playing"), ""); return; }
  if (lyricState == LYR_LOADING) return;
  long pos = currentPositionMs() + LYRIC_OFFSET_MS;
  if (lyricState == LYR_INSTRUMENTAL) { lcdShow(equalizerRow(pos), centered(String(C_NOTE) + " Instrumental"));  return; }
  if (lyricState != LYR_OK) { lcdShow(equalizerRow(pos), songName); return; }

  int idx = -1;
  for (int i = 0; i < lyricCount; i++) {
    if (lyricTime[i] <= pos) idx = i;
    else break;
  }
  if (LCD_STYLE == 0) lcdClassic(pos, idx);
  else lcdKaraoke(pos, idx);
}

void lcdKaraoke(long pos, int idx) {
  long nextTime = (idx + 1 < lyricCount) ? lyricTime[idx + 1] : -1;

  // Intro or music break: equalizer on top, song name (or countdown) below
  if (idx < 0 || isBreak(lyricText[idx])) {
    String bottom = songName;
    if (nextTime >= 0 && nextTime - pos <= 3000) bottom = countdownRow(nextTime - pos);
    lcdShow(equalizerRow(pos), bottom);
    return;
  }

  // Wrap the line into rows of 16, show 2 rows (a "page") at a time
  const int MAXR = 8;
  String rows[MAXR];
  int n = wrapLine(lyricText[idx], rows, MAXR);
  if (n == 0) { lcdShow("", ""); return; }
  int pages = (n + 1) / 2;

  long lineDur = (nextTime >= 0) ? (nextTime - lyricTime[idx]) : 4000;
  if (lineDur < 800) lineDur = 800;
  if (lineDur > 12000) lineDur = 12000;
  long into = pos - lyricTime[idx];
  if (into < 0) into = 0;

  long pageDur = lineDur / pages;
  int page = (int)(into / pageDur);
  if (page >= pages) page = pages - 1;
  long intoPage = into - page * pageDur;

  String top = rows[page * 2];
  String bot = (page * 2 + 1 < n) ? rows[page * 2 + 1] : String("");
  // center each row on the screen
  String r0 = centered(top), r1 = centered(bot);

  // Typewriter: letters appear at singing speed (~14 per second), but always
  // finish within 75% of the time this page is on screen.
  int total = top.length() + bot.length();
  long typeDur = (long)total * 70;
  if (typeDur > pageDur * 3 / 4) typeDur = pageDur * 3 / 4;
  if (typeDur < 1) typeDur = 1;
  int shown = (intoPage >= typeDur) ? total : (int)(intoPage * total / typeDur);

  // hide letters not "sung" yet; a music note runs in front of the text while typing
  int pad0 = r0.length() - top.length(), pad1 = r1.length() - bot.length();
  String o0 = r0, o1 = r1;
  for (int i = 0; i < (int)top.length(); i++) if (i >= shown) o0.setCharAt(pad0 + i, ' ');
  for (int i = 0; i < (int)bot.length(); i++) if (top.length() + i >= (unsigned)shown) o1.setCharAt(pad1 + i, ' ');
  if (shown < total) {
    if (shown < (int)top.length()) o0.setCharAt(pad0 + shown, C_NOTE);
    else o1.setCharAt(pad1 + (shown - top.length()), C_NOTE);
  }
  lcdShow(o0, o1);
}

void lcdClassic(long pos, int idx) {
  if (idx < 0) { lcdShow("~ ~ ~", lyricCount ? lyricText[0] : String("")); return; }

  const int MAXR = 8;
  String rows[MAXR];
  int n = wrapLine(lyricText[idx], rows, MAXR);
  String next = (idx + 1 < lyricCount) ? lyricText[idx + 1] : String("");

  if (n <= 1) {
    lcdShow(n ? rows[0] : String(""), next);
  } else if (n == 2) {
    lcdShow(rows[0], rows[1]);
  } else {
    // how long this line lasts in the song
    long lineDur = (idx + 1 < lyricCount) ? (lyricTime[idx + 1] - lyricTime[idx]) : 4000;
    if (lineDur < 1000) lineDur = 1000;
    if (lineDur > 10000) lineDur = 10000;
    int pages = (n + 1) / 2;
    long into = pos - lyricTime[idx];
    if (into < 0) into = 0;
    int page = (int)(into * pages / lineDur);
    if (page >= pages) page = pages - 1;
    String r1 = (page * 2 + 1 < n) ? rows[page * 2 + 1] : String("");
    lcdShow(rows[page * 2], r1);
  }
}

// ---------- Spotify ----------

bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, "https://accounts.spotify.com/api/token")) return false;
  String auth = base64::encode(String(CLIENT_ID) + ":" + CLIENT_SECRET);
  https.addHeader("Authorization", "Basic " + auth);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = https.POST(String("grant_type=refresh_token&refresh_token=") + REFRESH_TOKEN);
  if (code != 200) {
    Serial.printf("Token error %d: %s\n", code, https.getString().c_str());
    https.end();
    return false;
  }
  JsonDocument doc;
  deserializeJson(doc, https.getString());
  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAt = millis() + (unsigned long)(expiresIn - 60) * 1000UL;
  https.end();
  Serial.println("Got new access token");
  return true;
}

void fetchNowPlaying() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.spotify.com/v1/me/player/currently-playing");
  https.addHeader("Authorization", "Bearer " + accessToken);
  int code = https.GET();
  if (code == 200) {
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["item"]["id"] = true;
    filter["item"]["name"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["artists"][0]["name"] = true;
    filter["item"]["album"]["name"] = true;
    filter["item"]["album"]["images"][0]["url"] = true;
    filter["item"]["album"]["images"][0]["width"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, https.getString(), DeserializationOption::Filter(filter));
    if (!err && !doc["item"].isNull()) {
      lockLyrics();
      rawSong    = doc["item"]["name"].as<String>();
      rawArtist  = doc["item"]["artists"][0]["name"].as<String>();
      rawAlbum   = doc["item"]["album"]["name"].as<String>();
      songName   = cleanText(rawSong);
      artistName = cleanText(rawArtist);
      if (songName.length() == 0) songName = "?";
      if (artistName.length() == 0) artistName = "?";
      unlockLyrics();
      durationMs = doc["item"]["duration_ms"] | 0;
      progressMs = doc["progress_ms"] | 0;
      isPlaying  = doc["is_playing"] | false;
      hasTrack = true;

      String id = doc["item"]["id"].as<String>();
      if (id != currentTrackId) {        // new song -> get its lyrics
        currentTrackId = id;
        lyricsPending = true;
      }

      JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();
      String bestUrl = "";
      int bestW = 100000;
      for (JsonObject img : images) {
        int w = img["width"] | 0;
        String u = img["url"].as<String>();
        if (w > 0 && w < bestW && u.length() > 0) { bestW = w; bestUrl = u; }
      }
      if (bestUrl.length() > 0 && bestUrl != currentArtUrl) {
        pendingArtUrl = bestUrl;
        pendingArtWidth = bestW;
      }
    } else {
      hasTrack = false;
    }
  } else if (code == 204) {
    hasTrack = false;
  } else if (code == 401) {
    tokenExpiresAt = 0;
  } else {
    Serial.printf("Player error %d\n", code);
  }
  https.end();
  lastFetchMillis = millis();
  fetchedOnce = true;
}

// ---------- buttons ----------

int spotifyCommand(const char* method, const char* url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return -1;
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Content-Length", "0");
  int code = (strcmp(method, "PUT") == 0) ? https.PUT("") : https.POST("");
  if (code != 200 && code != 204) Serial.printf("Command error %d: %s\n", code, https.getString().c_str());
  https.end();
  return code;
}

void flash(const String& text) {
  flashText = text;
  flashUntil = millis() + 1200;
}

void onButton(int index) {
  int code = 0;
  if (index == 0) {
    flash("<< Prev");
    code = spotifyCommand("POST", "https://api.spotify.com/v1/me/player/previous");
  } else if (index == 1) {
    if (isPlaying) {
      flash("Pause");
      code = spotifyCommand("PUT", "https://api.spotify.com/v1/me/player/pause");
    } else {
      flash("Play");
      code = spotifyCommand("PUT", "https://api.spotify.com/v1/me/player/play");
    }
    if (code == 200 || code == 204) isPlaying = !isPlaying;
  } else {
    flash("Next >>");
    code = spotifyCommand("POST", "https://api.spotify.com/v1/me/player/next");
  }
  if (code == 401) tokenExpiresAt = 0;
  if (code == 403) flash("Need new token");
  if (code == 404) flash("Open Spotify");
  lastFetchMillis = millis() - 2500;   // refresh song info soon
}

void checkButtons() {
  for (int i = 0; i < 3; i++) {
    bool state = digitalRead(buttonPins[i]);
    if (state != lastButtonState[i] && millis() - lastButtonChange[i] > 50) {
      lastButtonChange[i] = millis();
      lastButtonState[i] = state;
      if (state == LOW) onButton(i);
    }
  }
}

// ---------- OLED drawing ----------

void drawPlaceholderArt() {
  display.fillRect(0, 0, ART, ART, SSD1306_BLACK);
  display.drawRect(0, 0, ART, ART, SSD1306_WHITE);
  display.fillCircle(24, 42, 6, SSD1306_WHITE);
  display.fillRect(29, 16, 3, 27, SSD1306_WHITE);
  display.fillRect(29, 16, 14, 4, SSD1306_WHITE);
}

void drawScreen() {
  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (!hasTrack) {
    drawPlaceholderArt();
    display.setCursor(TEXT_X, 20);
    display.print(fetchedOnce ? "Nothing" : "Loading");
    display.setCursor(TEXT_X, 32);
    display.print(fetchedOnce ? "playing" : "...");
    oledFlush();
    return;
  }

  int songW = songName.length() * 6;
  if (songW <= TEXT_W) {
    display.setCursor(TEXT_X, 4);
    display.print(songName);
  } else {
    int gap = 24;
    int offset = (millis() / 45) % (songW + gap);
    display.setCursor(TEXT_X - offset, 4);
    display.print(songName);
    display.setCursor(TEXT_X - offset + songW + gap, 4);
    display.print(songName);
  }

  display.setCursor(TEXT_X, 17);
  if (artistName.length() > 10) display.print(artistName.substring(0, 9) + ".");
  else display.print(artistName);

  if (millis() < flashUntil) {
    display.setCursor(TEXT_X, 30);
    display.print(flashText);
  } else if (isPlaying) {
    display.fillTriangle(TEXT_X, 29, TEXT_X, 37, TEXT_X + 7, 33, SSD1306_WHITE);
  } else {
    display.fillRect(TEXT_X, 29, 3, 9, SSD1306_WHITE);
    display.fillRect(TEXT_X + 5, 29, 3, 9, SSD1306_WHITE);
  }

  long shown = currentPositionMs();
  display.drawRect(TEXT_X, 43, TEXT_W, 5, SSD1306_WHITE);
  if (durationMs > 0) {
    int fill = (int)((shown * (long)(TEXT_W - 2)) / durationMs);
    display.fillRect(TEXT_X + 1, 44, fill, 3, SSD1306_WHITE);
  }
  String left = formatTime(shown);
  String right = formatTime(durationMs);
  display.setCursor(TEXT_X, 54);
  display.print(left);
  display.setCursor(128 - right.length() * 6, 54);
  display.print(right);

  if (hasArt) display.drawBitmap(0, 0, artBitmap, ART, ART, SSD1306_WHITE, SSD1306_BLACK);
  else drawPlaceholderArt();

  oledFlush();
}

// ---------- main ----------

void setup() {
  Serial.begin(115200);
  delay(1000);
  i2cLock = xSemaphoreCreateMutex();
  lyricsLock = xSemaphoreCreateMutex();
  if (!setupDisplay()) {
    Serial.println("OLED NOT FOUND - check wires");
    while (true) delay(1000);
  }
  setupLCD();
  for (int i = 0; i < 3; i++) pinMode(buttonPins[i], INPUT_PULLUP);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpgToGray);
  showMessage("Starting...");
  connectWiFi();
  if (lcd) xTaskCreatePinnedToCore(lcdTask, "lcd", 6144, nullptr, 1, nullptr, 0);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    delay(2000);
    return;
  }

  if (accessToken == "" || (long)(millis() - tokenExpiresAt) >= 0) {
    showMessage("Logging in to", "Spotify...");
    lcdShow("Logging in...", "");
    if (!refreshAccessToken()) {
      showMessage("Spotify login error", "See Serial Monitor");
      lcdShow("Login error", "");
      delay(10000);
      return;
    }
  }

  checkButtons();

  if (!fetchedOnce || millis() - lastFetchMillis > 3000) fetchNowPlaying();

  if (pendingArtUrl.length() > 0) {
    String url = pendingArtUrl;
    pendingArtUrl = "";
    hasArt = loadAlbumArt(url, pendingArtWidth);
    currentArtUrl = url;
  }

  if (lyricsPending && hasTrack) {
    lyricsPending = false;
    if (lcd) loadLyrics();
  }

  drawScreen();
  delay(30);
}
