#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Cheap Yellow Display (ESP32-2432S028R) Hardware Pins & Calibration
// ---------------------------------------------------------------------------
#define XPT2046_CLK   25
#define XPT2046_MISO  39
#define XPT2046_MOSI  32
#define XPT2046_CS    33
#define XPT2046_IRQ   36

// Raw Touch Calibration Limits (320x240 Rotation 1)
#define TS_MINX 200
#define TS_MAXX 3700
#define TS_MINY 240
#define TS_MAXY 3800

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// Built-in TFT_eSPI FreeFont macros
#define FSS9   &FreeSans9pt7b
#define FSS12  &FreeSans12pt7b
#define FSB9   &FreeSansBold9pt7b
#define FSB18  &FreeSansBold18pt7b

// WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

// Target WPSD live caller endpoint & Host definition
const char* hostUrl   = "http://123.123.123.123";
const char* serverUrl = "http://123.123.123.123/mmdvmhost/live_caller_backend.php";

TFT_eSPI tft = TFT_eSPI();
PNG png;

unsigned long lastFetchTime = 0;
const unsigned long interval = 1500; // Poll every 1.5s

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 5000;

struct CallerData {
  String callsign = "";
  String dmrId = "";
  String name = "";
  String mode = "";
  String target = "";
  String duration = "";
  String flagPath = "";
};

CallerData lastData;
bool haveRenderedOnce = false;

// Buffer to download PNG file into RAM
uint8_t* pngBuffer = NULL;
size_t pngBufferSize = 0;

// Network clients
WiFiClient localClient;
WiFiClientSecure radioIdClient;
bool radioIdClientReady = false;

// Screen layout constants (Expanded interline spacing & generous gap below Duration)
const int SCREEN_W  = 320;
const int BANNER_Y  = 26;
const int BANNER_H  = 60;
const int NAME_Y    = 104;
const int DIVIDER_Y = 114;
const int META_X    = 10;
const int META1_Y   = 132; // DMR ID
const int META2_Y   = 153; // Target (+21 spacing)
const int META3_Y   = 174; // Mode (+21 spacing)
const int META4_Y   = 195; // Duration (+21 spacing, extra lower padding)
const int META5_Y   = 230; // Clock (Bottom line safely spaced with generous gap)

uint16_t wpsdOrange;

// ---------------------------------------------------------------------------
// Theme (Light / Dark Mode) & Auto Sunrise/Sunset Configuration
// ---------------------------------------------------------------------------
bool darkMode = false;
uint16_t bgColor    = TFT_WHITE;
uint16_t fgColor    = TFT_BLACK;
uint16_t labelColor = TFT_DARKGREY;

// Location coordinates for automatic Sunrise/Sunset calculation (Default: Rome, Italy)
const double OBS_LATITUDE = 41.9028;
const double OBS_LONGITUDE = 12.4964;

void updateThemeColors() {
  if (darkMode) {
    bgColor    = TFT_BLACK;
    fgColor    = TFT_WHITE;
    labelColor = TFT_LIGHTGREY;
  } else {
    bgColor    = TFT_WHITE;
    fgColor    = TFT_BLACK;
    labelColor = TFT_DARKGREY;
  }
}

unsigned long lastThemeToggle = 0;
const unsigned long toggleDebounceMs = 400;

// ---------------------------------------------------------------------------
// UTC Clock (NTP) & Solar Calculations
// ---------------------------------------------------------------------------
const char* ntpServer = "0.pool.ntp.org";
unsigned long lastNtpSync = 0;
const unsigned long ntpSyncInterval = 3600000UL;

unsigned long lastClockUpdate = 0;
const unsigned long clockUpdateInterval = 1000;

char lastClockText[40] = "";

void updateClockDisplay(bool force = false);
void toggleDarkMode();
void checkTouchToggle();
void checkAutoSunTheme(time_t now);

// ---------------------------------------------------------------------------
// Helpers & HTML Parsing
// ---------------------------------------------------------------------------
String stripTags(const String& text) {
  String clean;
  clean.reserve(text.length());
  bool inTag = false;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '<') inTag = true;
    else if (c == '>') inTag = false;
    else if (!inTag && c != '\r' && c != '\n' && c != '\t') clean += c;
  }
  clean.trim();
  return clean;
}

String extractBetween(const String& html, const String& startStr, const String& endStr, int fromIndex = 0) {
  int start = html.indexOf(startStr, fromIndex);
  if (start == -1) return "";
  start += startStr.length();
  int end = html.indexOf(endStr, start);
  if (end == -1) return "";
  return html.substring(start, end);
}

String extractTagWithClass(const String& html, const String& className, int fromIndex = 0) {
  int pos = fromIndex;
  while ((pos = html.indexOf(className, pos)) != -1) {
    int closeTag = html.indexOf('>', pos);
    if (closeTag != -1) {
      int endTag = html.indexOf("</", closeTag);
      if (endTag != -1 && endTag > closeTag) {
        String result = stripTags(html.substring(closeTag + 1, endTag));
        if (result.length() > 0) return result;
      }
    }
    pos += className.length();
  }
  return "";
}

String getDmrIdForCallsign(const String& callsign) {
  if (callsign.length() == 0 || callsign == "LISTENING...") return "";

  static String cachedCallsign = "";
  static String cachedDmrId = "";
  static bool cachedIsNegative = false;

  if (callsign == cachedCallsign) {
    return cachedIsNegative ? "" : cachedDmrId;
  }

  if (!radioIdClientReady) {
    radioIdClient.setInsecure();
    radioIdClientReady = true;
  }

  HTTPClient http;
  String url = "https://radioid.net/api/users?callsign=" + callsign;
  http.begin(radioIdClient, url);
  http.addHeader("Connection", "close");
  http.setConnectTimeout(800);
  http.setTimeout(1500);

  String result = "";
  bool negative = true;

  int httpCode = http.GET();
  if (httpCode < 0) {
    radioIdClient.stop();
  } else if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int idIdx = payload.indexOf("\"radio_id\":");
    if (idIdx != -1) {
      int start = idIdx + 11;
      while (start < (int)payload.length() && !isDigit(payload[start])) start++;
      String idStr;
      while (start < (int)payload.length() && isDigit(payload[start])) {
        idStr += payload[start];
        start++;
      }
      if (idStr.length() >= 6 && idStr.length() <= 8) {
        result = idStr;
        negative = false;
      }
    }
  }
  http.end();

  cachedCallsign = callsign;
  cachedDmrId = result;
  cachedIsNegative = negative;
  return result;
}

String extractDmrId(const String& html, const String& callsign) {
  if (html.length() == 0) return "";
  const char* idClasses[] = {"oc_id", "dc_id", "dmr_id", "caller_id", "radio_id"};
  for (int c = 0; c < 5; c++) {
    String tagVal = extractTagWithClass(html, idClasses[c]);
    String digits;
    for (size_t i = 0; i < tagVal.length(); i++) {
      if (isDigit(tagVal[i])) digits += tagVal[i];
    }
    if (digits.length() >= 6 && digits.length() <= 8) return digits;
  }
  return getDmrIdForCallsign(callsign);
}

// ---------------------------------------------------------------------------
// PNG Decoder
// ---------------------------------------------------------------------------
uint16_t pngLineBuf[SCREEN_W];
uint16_t pngScaledBuf[SCREEN_W / 5 + 1];

int pngDrawCallback(PNGDRAW *pDraw) {
  if (pDraw->iWidth <= 0 || pDraw->iWidth > SCREEN_W) return 1;

  png.getLineAsRGB565(pDraw, pngLineBuf, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

  if (pDraw->y % 5 == 0) {
    int scaledWidth = pDraw->iWidth / 5;
    if (scaledWidth > (int)(sizeof(pngScaledBuf) / sizeof(pngScaledBuf[0]))) {
      scaledWidth = sizeof(pngScaledBuf) / sizeof(pngScaledBuf[0]);
    }
    int scaledHeight = png.getHeight() / 5;

    for (int i = 0; i < scaledWidth; i++) {
      pngScaledBuf[i] = pngLineBuf[i * 5];
    }

    int startX = 250;
    int startY = BANNER_Y + ((BANNER_H - scaledHeight) / 2) + (pDraw->y / 5);

    if (startX + scaledWidth <= SCREEN_W && startY < BANNER_Y + BANNER_H) {
      tft.pushImage(startX, startY, scaledWidth, 1, pngScaledBuf);
    }
  }
  return 1;
}

bool fetchPNGImage(const String& relativePath) {
  if (WiFi.status() != WL_CONNECTED || relativePath.length() == 0) return false;

  String fullUrl = String(hostUrl) + relativePath;
  HTTPClient http;
  http.begin(localClient, fullUrl);
  http.addHeader("Connection", "close");
  http.setConnectTimeout(800);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool ok = false;
  int httpCode = http.GET();
  
  if (httpCode < 0) {
    localClient.stop();
  } else if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();
    if (len > 0) {
      uint8_t* newBuf = (uint8_t*)realloc(pngBuffer, len);
      if (newBuf != NULL) {
        pngBuffer = newBuf;
        WiFiClient* stream = http.getStreamPtr();
        int bytesRead = 0;
        unsigned long startMs = millis();
        while (http.connected() && bytesRead < len && (millis() - startMs) < 2000) {
          size_t available = stream->available();
          if (available) {
            int c = stream->readBytes(pngBuffer + bytesRead, available);
            bytesRead += c;
          } else {
            delay(1);
          }
        }
        pngBufferSize = bytesRead;
        ok = (bytesRead == len);
      }
    }
  }
  http.end();
  return ok;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void drawStaticChrome() {
  tft.fillScreen(bgColor);
  // Top bar styled in Dark Gray with White centered text
  tft.fillRect(0, 0, SCREEN_W, 24, TFT_DARKGREY);
  tft.setFreeFont(FSS9);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  
  String title = "WPSD HOTSPOT LIVE CALLER";
  int textWidth = tft.textWidth(title);
  int startX = (SCREEN_W - textWidth) / 2;
  if (startX < 0) startX = 0;
  
  tft.setCursor(startX, 17);
  tft.print(title);
  
  tft.drawFastHLine(0, DIVIDER_Y, SCREEN_W, TFT_LIGHTGREY);
}

void drawFlagFromBuffer() {
  if (pngBuffer == NULL || pngBufferSize == 0) return;
  int rc = png.openRAM(pngBuffer, pngBufferSize, pngDrawCallback);
  if (rc == PNG_SUCCESS) {
    if (png.getWidth() > 0 && png.getWidth() <= SCREEN_W) {
      png.decode(NULL, 0);
    }
    png.close();
  }
}

void drawCallsignBanner(const CallerData& data, bool fetchNewFlag) {
  tft.fillRect(0, BANNER_Y, SCREEN_W, BANNER_H, wpsdOrange);

  if (data.callsign.length() > 0) {
    tft.setFreeFont(FSB18);
    tft.setTextColor(TFT_BLACK, wpsdOrange);
    tft.setCursor(15, 68);
    tft.print(data.callsign);
  } else {
    tft.setFreeFont(FSS12);
    tft.setTextColor(TFT_BLACK, wpsdOrange);
    tft.setCursor(15, 63);
    tft.print("LISTENING...");
  }

  if (fetchNewFlag) {
    if (data.flagPath.length() > 0 && fetchPNGImage(data.flagPath)) {
      drawFlagFromBuffer();
    }
  } else {
    drawFlagFromBuffer();
  }
}

void drawNameLine(const CallerData& data) {
  tft.fillRect(META_X, NAME_Y - 18, SCREEN_W - META_X, 24, bgColor);
  tft.setFreeFont(FSB9);
  tft.setTextColor(fgColor, bgColor);
  tft.setCursor(META_X, NAME_Y);
  tft.print(data.name.length() > 0 ? data.name : "No Active Transmission");
}

void drawMetaLine(int y, const char* label, const String& value) {
  tft.fillRect(META_X, y - 13, SCREEN_W - META_X, 20, bgColor);
  tft.setFreeFont(FSS9);
  tft.setCursor(META_X, y);
  tft.setTextColor(labelColor, bgColor);
  tft.print(label);
  tft.setTextColor(fgColor, bgColor);
  tft.print(value);
}

void renderDashboard(const CallerData& data) {
  bool callsignChanged = !haveRenderedOnce || data.callsign != lastData.callsign || data.flagPath != lastData.flagPath;
  bool nameChanged     = !haveRenderedOnce || data.name != lastData.name;
  bool dmrChanged      = !haveRenderedOnce || data.dmrId != lastData.dmrId;
  bool targetChanged   = !haveRenderedOnce || data.target != lastData.target;
  bool modeChanged     = !haveRenderedOnce || data.mode != lastData.mode;
  bool durationChanged = !haveRenderedOnce || data.duration != lastData.duration;

  if (!haveRenderedOnce) {
    drawStaticChrome();
  }

  if (callsignChanged) drawCallsignBanner(data, true);
  if (nameChanged)     drawNameLine(data);
  if (dmrChanged)      drawMetaLine(META1_Y, "DMR ID: ", data.dmrId.length() > 0 ? data.dmrId : "N/A");
  if (targetChanged)   drawMetaLine(META2_Y, "Target: ", data.target);
  if (modeChanged)     drawMetaLine(META3_Y, "Mode: ", data.mode);
  if (durationChanged) drawMetaLine(META4_Y, "Duration: ", data.duration);

  if (!haveRenderedOnce) {
    updateClockDisplay(true);
  }

  haveRenderedOnce = true;
}

void syncTimeWithNTP() {
  configTime(0, 0, ntpServer);
  lastNtpSync = millis();
}

void drawClockLine(const String& text) {
  tft.fillRect(0, META5_Y - 10, SCREEN_W, 14, bgColor);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(fgColor, bgColor);

  int textW = tft.textWidth(text);
  int x = (SCREEN_W - textW) / 2;
  if (x < 0) x = 0;

  tft.setCursor(x, META5_Y - 6);
  tft.print(text);
}

void updateClockDisplay(bool force) {
  time_t now = time(nullptr);
  char buf[40];

  if (now < 1700000000L) {
    snprintf(buf, sizeof(buf), "Syncing time...");
  } else {
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), "%Y/%b/%d - %H:%M:%S - UTC", &timeinfo);
    
    // Check and apply automatic sunrise/sunset theme switching on solar events
    checkAutoSunTheme(now);
  }

  if (!force && strcmp(buf, lastClockText) == 0) return;

  strncpy(lastClockText, buf, sizeof(lastClockText));
  drawClockLine(String(buf));
}

void redrawAfterThemeChange() {
  drawStaticChrome();
  drawCallsignBanner(lastData, false);
  drawNameLine(lastData);
  drawMetaLine(META1_Y, "DMR ID: ", lastData.dmrId.length() > 0 ? lastData.dmrId : "N/A");
  drawMetaLine(META2_Y, "Target: ", lastData.target);
  drawMetaLine(META3_Y, "Mode: ", lastData.mode);
  drawMetaLine(META4_Y, "Duration: ", lastData.duration);
  updateClockDisplay(true);
}

void toggleDarkMode() {
  darkMode = !darkMode;
  updateThemeColors();
  redrawAfterThemeChange();
}

// ---------------------------------------------------------------------------
// Automatic Sunrise / Sunset Theme Switcher (Triggers only at transitions)
// ---------------------------------------------------------------------------
void getSunRiseSetTimes(int year, int month, int day, double lat, double lon, double &sunriseUTC, double &sunsetUTC) {
  int d = 367 * year - (7 * (year + (month + 9) / 12)) / 4 + (275 * month) / 9 + day - 730531.5;
  double wgt = 282.9404 + 4.70935E-5 * d;
  double ecc = 0.016709 - 1.151E-9 * d;
  double man = fmod(356.0470 + 0.9856002585 * d, 360.0);
  double obl = 23.4393 - 3.563E-7 * d;
  
  double e_rad = man * M_PI / 180.0;
  double sur = man + (180.0 / M_PI) * ecc * 2.0 * sin(e_rad);
  double sur_rad = sur * M_PI / 180.0;
  
  double x = cos(sur_rad) - ecc;
  double y = sin(sur_rad) * cos(obl * M_PI / 180.0);
  double v = atan2(y, x) * 180.0 / M_PI;
  double lon_sun = fmod(v + wgt, 360.0);
  
  double decl = asin(sin(obl * M_PI / 180.0) * sin(lon_sun * M_PI / 180.0)) * 180.0 / M_PI;
  double l_quad = floor(lon_sun / 90.0) * 90.0;
  double r_quad = floor(v / 90.0) * 90.0;
  double ra = (atan2(cos(obl * M_PI / 180.0) * sin(lon_sun * M_PI / 180.0), cos(lon_sun * M_PI / 180.0)) * 180.0 / M_PI + l_quad - r_quad) / 15.0;
  
  double gmst = fmod(280.1606 + 360.9856237 * d, 360.0) / 15.0;
  double lha_sun = cos(90.833 * M_PI / 180.0) / (cos(lat * M_PI / 180.0) * cos(decl * M_PI / 180.0)) - tan(lat * M_PI / 180.0) * tan(decl * M_PI / 180.0);
  
  if (lha_sun > 1.0 || lha_sun < -1.0) {
    sunriseUTC = 6.0;
    sunsetUTC = 18.0;
    return;
  }
  
  double h_angle = acos(lha_sun) * 180.0 / M_PI / 15.0;
  double ut_noon = ra - lon / 15.0 - gmst;
  ut_noon = fmod(ut_noon + 48.0, 24.0);
  
  sunriseUTC = fmod(ut_noon - h_angle + 24.0, 24.0);
  sunsetUTC = fmod(ut_noon + h_angle + 24.0, 24.0);
}

void checkAutoSunTheme(time_t now) {
  struct tm tinfo;
  gmtime_r(&now, &tinfo);

  double sunriseUTC = 6.0;
  double sunsetUTC = 18.0;
  getSunRiseSetTimes(tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday, OBS_LATITUDE, OBS_LONGITUDE, sunriseUTC, sunsetUTC);

  double currentUTC = tinfo.tm_hour + (tinfo.tm_min / 60.0) + (tinfo.tm_sec / 3600.0);

  bool shouldBeDark = (currentUTC < sunriseUTC || currentUTC >= sunsetUTC);

  static bool lastCalculatedDark = shouldBeDark;
  static bool initializedSun = false;

  if (!initializedSun) {
    darkMode = shouldBeDark;
    lastCalculatedDark = shouldBeDark;
    updateThemeColors();
    initializedSun = true;
    return;
  }

  // Only auto-switch when a transition (Sunrise or Sunset) occurs, allowing manual tap overrides
  if (shouldBeDark != lastCalculatedDark) {
    lastCalculatedDark = shouldBeDark;
    darkMode = shouldBeDark;
    updateThemeColors();
    redrawAfterThemeChange();
  }
}

// ---------------------------------------------------------------------------
// Invisible Banner Touch Region
// ---------------------------------------------------------------------------
void checkTouchToggle() {
  if (!ts.touched()) return;

  TS_Point p = ts.getPoint();

  if (p.z < 60) return;

  int touchY = map(p.y, TS_MINY, TS_MAXY, 0, 240);

  if (touchY >= 20 && touchY <= 90) {
    if (millis() - lastThemeToggle > toggleDebounceMs) {
      lastThemeToggle = millis();
      toggleDarkMode();
    }
  }
}

// ---------------------------------------------------------------------------
// Network Request Handling
// ---------------------------------------------------------------------------
CallerData parseWPSDHtml(const String& html) {
  CallerData data;

  // 1. Try standard WPSD classes
  data.callsign = extractTagWithClass(html, "oc_call");
  if (data.callsign.length() == 0) data.callsign = extractTagWithClass(html, "callsign");
  if (data.callsign.length() == 0) data.callsign = extractBetween(html, "class='oc_call'>", "</span>");
  if (data.callsign.length() == 0) data.callsign = extractBetween(html, "class=\"oc_call\">", "</span>");
  data.callsign.trim();

  // 2. Try Name fields
  data.name = extractTagWithClass(html, "oc_name");
  if (data.name.length() == 0) data.name = extractTagWithClass(html, "name");
  data.name = stripTags(data.name);

  // 3. DMR ID lookup fallback
  data.dmrId = extractDmrId(html, data.callsign);

  // 4. Flag path extraction
  data.flagPath = extractBetween(html, "<img src='", "'");
  if (data.flagPath.length() == 0) {
    data.flagPath = extractBetween(html, "<img src=\"", "\"");
  }

  int queryIdx = data.flagPath.indexOf('?');
  if (queryIdx != -1) {
    data.flagPath = data.flagPath.substring(0, queryIdx);
  }
  if (!data.flagPath.endsWith(".png")) {
    data.flagPath = "";
  }

  // 5. Mode, Target, and Duration extraction handles multiple variations
  int modeIdx = html.indexOf("Mode:");
  if (modeIdx == -1) modeIdx = html.indexOf("mode:");
  if (modeIdx != -1) {
    data.mode = extractTagWithClass(html, "dc_info_def", modeIdx);
    if (data.mode.length() == 0) data.mode = extractBetween(html, "class='dc_info_def'>", "</span>", modeIdx);
    data.mode = stripTags(data.mode);
  }

  int targetIdx = html.indexOf("Target:");
  if (targetIdx == -1) targetIdx = html.indexOf("target:");
  if (targetIdx != -1) {
    data.target = extractTagWithClass(html, "dc_info_def", targetIdx);
    if (data.target.length() == 0) data.target = extractBetween(html, "class='dc_info_def'>", "</span>", targetIdx);
    data.target = stripTags(data.target);
  }

  int durIdx = html.indexOf("TX Duration:");
  if (durIdx == -1) durIdx = html.indexOf("Duration:");
  if (durIdx != -1) {
    data.duration = extractTagWithClass(html, "dc_info_def", durIdx);
    if (data.duration.length() == 0) data.duration = extractBetween(html, "class='dc_info_def'>", "</span>", durIdx);
    data.duration = stripTags(data.duration);
  }

  return data;
}

void fetchAndDisplayPage() {
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(bgColor);
    tft.setFreeFont(FSS9);
    tft.setCursor(10, 30);
    tft.setTextColor(TFT_RED, bgColor);
    tft.print("WiFi Disconnected");
    haveRenderedOnce = false;
    return;
  }

  if (!haveRenderedOnce) {
    renderDashboard(lastData);
  }

  HTTPClient http;
  http.begin(localClient, serverUrl);
  http.addHeader("Connection", "close");
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpResponseCode = http.GET();

  checkTouchToggle();

  if (httpResponseCode == HTTP_CODE_OK) {
    String rawHtml = http.getString();
    
    CallerData currentData = parseWPSDHtml(rawHtml);

    if (currentData.callsign != lastData.callsign ||
        currentData.duration != lastData.duration ||
        currentData.name     != lastData.name     ||
        currentData.target   != lastData.target   ||
        currentData.mode     != lastData.mode     ||
        currentData.dmrId    != lastData.dmrId) {
      renderDashboard(currentData);
      lastData = currentData;
    }
  } else {
    if (httpResponseCode < 0) {
      localClient.stop();
    }
  }
  http.end();
}

void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiCheck < wifiCheckInterval) return;
  lastWifiCheck = now;

  WiFi.disconnect();
  WiFi.begin(ssid, password);
}

// ---------------------------------------------------------------------------
// Setup & Loop
// ---------------------------------------------------------------------------
void setup() {
  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(true);
  tft.setSwapBytes(false);

  updateThemeColors();
  tft.fillScreen(bgColor);

  wpsdOrange = tft.color565(226, 140, 25);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  // Initialize XPT2046 touch screen over VSPI
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);
  pinMode(XPT2046_IRQ, INPUT);

  tft.setFreeFont(FSS9);
  tft.setTextColor(fgColor, bgColor);
  tft.setCursor(10, 30);
  tft.print("Connecting WiFi...");

  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 15000) {
    delay(250);
  }

  tft.fillScreen(bgColor);
  tft.setCursor(10, 30);
  tft.print(WiFi.status() == WL_CONNECTED ? "WiFi Connected!" : "WiFi Connect Failed - retrying...");
  delay(500);

  if (WiFi.status() == WL_CONNECTED) {
    syncTimeWithNTP();
  }
}

void loop() {
  checkTouchToggle();

  ensureWifiConnected();

  unsigned long currentMillis = millis();

  if (currentMillis - lastFetchTime >= interval) {
    lastFetchTime = currentMillis;
    fetchAndDisplayPage();
    checkTouchToggle();
  }

  if (currentMillis - lastClockUpdate >= clockUpdateInterval) {
    lastClockUpdate = currentMillis;
    updateClockDisplay();
  }

  if (WiFi.status() == WL_CONNECTED && currentMillis - lastNtpSync >= ntpSyncInterval) {
    syncTimeWithNTP();
  }
}
