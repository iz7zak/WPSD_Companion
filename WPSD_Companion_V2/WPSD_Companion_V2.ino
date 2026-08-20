#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

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
#define SSID "YOUR_SSID"
#define PASSWORD "YOUR_WIFI_PASSWORD"

// Target WPSD live caller endpoint & Host definition
#define HOST_URL   "http://123.123.123.123"
#define SERVER_URL "http://123.123.123.123/mmdvmhost/live_caller_backend.php"

// Callsign API
#define CALLSIGN_API_URL "https://radioid.net/api/users?callsign="

PNG png;

unsigned long lastFetchTime = 0;
#define INTERVAL 1500 // Poll every 1.5s
#define CONNECT_TIMEOUT 800

unsigned long lastWifiCheck = 0;
#define WIFI_CHECK_INTERVAL 5000

typedef struct CallerData {
  char callsign[32];
  char dmrId[16];
  char name[64];
  char mode[16];
  char target[32];
  char duration[16];
  char flagPath[128];
} CallerData;

CallerData lastData = {};
bool haveRenderedOnce = false;

// Buffer to download PNG file into RAM
uint8_t* pngBuffer = NULL;
size_t pngBufferSize = 0;

// Network clients
WiFiClient localClient;
WiFiClientSecure radioIdClient;
bool radioIdClientReady = false;

// Screen layout constants (Expanded interline spacing & generous gap below Duration)
#define SCREEN_W  320
#define SCREEN_H  320
#define BANNER_Y  26
#define BANNER_H  60
#define NAME_Y    104
#define DIVIDER_Y 114
#define META_X    10
#define META1_Y   132 // DMR ID
#define META2_Y   153 // Target (+21 spacing)
#define META3_Y   174 // Mode (+21 spacing)
#define META4_Y   195 // Duration (+21 spacing, extra lower padding)
#define META5_Y   230 // Clock (Bottom line safely spaced with generous gap)

TFT_eSPI tft = TFT_eSPI(SCREEN_W, SCREEN_H);
static uint16_t wpsdOrange;

// ---------------------------------------------------------------------------
// Theme (Light / Dark Mode) & Auto Sunrise/Sunset Configuration
// ---------------------------------------------------------------------------
static bool darkMode = false;
static uint16_t bgColor    = TFT_WHITE;
static uint16_t fgColor    = TFT_BLACK;
static uint16_t labelColor = TFT_DARKGREY;

// Location coordinates for automatic Sunrise/Sunset calculation (Default: Rome, Italy)
#define OBS_LATITUDE 41.9028
#define OBS_LONGITUDE 12.4964

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

static unsigned long lastThemeToggle = 0;
#define TOGGLE_DEBOUNCE_MS 400

// ---------------------------------------------------------------------------
// UTC Clock (NTP) & Solar Calculations
// ---------------------------------------------------------------------------
#define NTP_SERVER "0.pool.ntp.org"
static unsigned long lastNtpSync = 0;
#define NTP_SYNC_INTERVAL 3600000UL

static unsigned long lastClockUpdate = 0;
#define CLOCK_UPDATE_INTERVAL 1000

static char lastClockText[40] = "";

void updateClockDisplay(bool force = false);
void toggleDarkMode();
void checkTouchToggle();
void checkAutoSunTheme(time_t now);

// ---------------------------------------------------------------------------
// Helpers & HTML Parsing
// ---------------------------------------------------------------------------
bool copySubstring(char* dst, size_t dstSize,
                   const char* start, const char* end) {
  if (!dst || dstSize == 0 || !start || !end || end < start) {
    if (dst && dstSize > 0) dst[0] = '\0';
    return false;
  }

  size_t length = end - start;
  if (length >= dstSize) length = dstSize - 1;

  memcpy(dst, start, length);
  dst[length] = '\0';
  return length > 0;
}

bool copyField(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) {
    return false;
  }

  if (!src) {
    dst[0] = '\0';
    return true;
  }

  size_t sourceLength = strlen(src);
  bool truncated = sourceLength >= dstSize;

  size_t copyLength = sourceLength;
  if (copyLength >= dstSize) {
    copyLength = dstSize - 1;
  }

  memcpy(dst, src, copyLength);
  dst[copyLength] = '\0';

  return !truncated;
}

void stripTags(const char* text, char* output, size_t outputSize) {
  if (!output || outputSize == 0) return;

  output[0] = '\0';
  if (!text) return;

  bool inTag = false;
  size_t out = 0;

  for (size_t i = 0; text[i] != '\0' && out < outputSize - 1; i++) {
    char c = text[i];

    if (c == '<') {
      inTag = true;
    } else if (c == '>') {
      inTag = false;
    } else if (!inTag &&
               c != '\r' &&
               c != '\n' &&
               c != '\t' &&
               c != ' ') {
      output[out++] = c;
    }
  }

  output[out] = '\0';
}

bool extractBetween(const char* html,
                    const char* startStr,
                    const char* endStr,
                    char* output,
                    size_t outputSize) {
  if (!output || outputSize == 0) return false;

  output[0] = '\0';

  if (!html || !startStr || !endStr) return false;

  const char* start = strstr(html, startStr);
  if (!start) return false;

  start += strlen(startStr);

  const char* end = strstr(start, endStr);
  if (!end) return false;

  return copySubstring(output, outputSize, start, end);
}

bool extractTagWithClass(
    const char* html,
    const char* className,
    char* output,
    size_t outputSize,
    const char* searchFrom = nullptr
  ) {
  if (!output || outputSize == 0) return false;

  output[0] = '\0';

  if (!html || !className) return false;

  const char* searchStart = searchFrom ? searchFrom : html;
  const char* classPos = strstr(searchStart, className);

  if (!classPos) return false;

  const char* contentStart = strchr(classPos, '>');
  if (!contentStart) return false;

  contentStart++;

  const char* contentEnd = strstr(contentStart, "</");
  if (!contentEnd) return false;

  char temporary[128];

  if (!copySubstring(
        temporary,
        sizeof(temporary),
        contentStart,
        contentEnd)) {
    return false;
  }

  stripTags(temporary, output, outputSize);
  return strlen(output) > 0;
}

char* getDmrIdForCallsign(const char* callsign) {
  static char cachedCallsign[32] = "";
  static char cachedDmrId[16] = "";

  if (!callsign ||
      strlen(callsign) == 0 ||
      strcmp(callsign, "LISTENING...") == 0) {
    return cachedDmrId;
  }

  if (strcmp(callsign, cachedCallsign) == 0) {
    return cachedDmrId;
  }

  cachedDmrId[0] = '\0';

  if (!radioIdClientReady) {
    radioIdClient.setInsecure();
    radioIdClientReady = true;
  }

  char url[192];
  snprintf(url, sizeof(url), "%s%s", CALLSIGN_API_URL, callsign);

  HTTPClient http;

  if (!http.begin(radioIdClient, url)) {
    copyField(cachedCallsign, sizeof(cachedCallsign), callsign);
    return cachedDmrId;
  }

  http.addHeader("Connection", "close");
  http.setConnectTimeout(CONNECT_TIMEOUT);
  http.setTimeout(INTERVAL);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    const char* payloadText = payload.c_str();

    const char* idPos = strstr(payloadText, "\"radio_id\"");
    if (idPos) {
      const char* colon = strchr(idPos, ':');

      if (colon) {
        const char* p = colon + 1;
        while (*p &&
               !isDigit(static_cast<unsigned char>(*p))) {
          p++;
        }

        size_t pos = 0;

        while (*p &&
               isDigit(static_cast<unsigned char>(*p)) &&
               pos < sizeof(cachedDmrId) - 1) {
          cachedDmrId[pos++] = *p++;
        }

        cachedDmrId[pos] = '\0';

        if (pos < 6 || pos > 8) {
          cachedDmrId[0] = '\0';
        }
      }
    }
  }

  http.end();

  copyField(cachedCallsign, sizeof(cachedCallsign), callsign);
  return cachedDmrId;
}

char* extractDmrId(const char* html, const char* callsign) {
  static char result[16] = "";
  result[0] = '\0';

  if (!html || strlen(html) == 0) {
    return getDmrIdForCallsign(callsign);
  }

  const char* idClasses[] = {
    "oc_id",
    "dc_id",
    "dmr_id",
    "caller_id",
    "radio_id"
  };

  char tagValue[64];

  for (int c = 0; c < 5; c++) {
    if (!extractTagWithClass(
          html,
          idClasses[c],
          tagValue,
          sizeof(tagValue))) {
      continue;
    }

    size_t pos = 0;

    for (size_t i = 0;
         tagValue[i] != '\0' &&
         pos < sizeof(result) - 1;
         i++) {
      if (isDigit(static_cast<unsigned char>(tagValue[i]))) {
        result[pos++] = tagValue[i];
      }
    }

    result[pos] = '\0';

    if (pos >= 6 && pos <= 8) {
      return result;
    }
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

bool fetchPNGImage(const char* relativePath) {
  if (WiFi.status() != WL_CONNECTED ||
      !relativePath ||
      strlen(relativePath) == 0) {
    return false;
  }

  char fullUrl[256];
  snprintf(
    fullUrl,
    sizeof(fullUrl),
    "%s%s",
    HOST_URL,
    relativePath
  );

  HTTPClient http;

  if (!http.begin(localClient, fullUrl)) {
    return false;
  }

  http.addHeader("Connection", "close");
  http.setConnectTimeout(CONNECT_TIMEOUT);
  http.setTimeout(INTERVAL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool ok = false;
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();

    if (len > 0 && len <= 32768) {
      uint8_t* newBuf = static_cast<uint8_t*>(
        realloc(pngBuffer, len)
      );

      if (newBuf) {
        pngBuffer = newBuf;

        WiFiClient* stream = http.getStreamPtr();
        int bytesRead = 0;
        unsigned long startMs = millis();

        while (http.connected() &&
               bytesRead < len &&
               millis() - startMs < 2000) {
          size_t available = stream->available();

          if (available > 0) {
            size_t remaining = len - bytesRead;
            if (available > remaining) {
              available = remaining;
            }

            int count = stream->readBytes(
              pngBuffer + bytesRead,
              available
            );

            if (count > 0) {
              bytesRead += count;
            }
          } else {
            delay(1);
          }
        }

        pngBufferSize = bytesRead;
        ok = bytesRead == len;
      }
    }
  } else if (httpCode < 0) {
    localClient.stop();
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
  
  const char* title = "WPSD HOTSPOT LIVE CALLER";
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

  if (strlen(data.callsign) > 0) {
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
    if (strlen(data.flagPath) > 0 && fetchPNGImage(data.flagPath)) {
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
  tft.print(strlen(data.name) > 0 ? data.name : "No Active Transmission");
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
  bool callsignChanged = !haveRenderedOnce || strcmp(data.callsign, lastData.callsign) != 0 || strcmp(data.flagPath, lastData.flagPath) != 0;
  bool nameChanged     = !haveRenderedOnce || strcmp(data.name, lastData.name) != 0;
  bool dmrChanged      = !haveRenderedOnce || strcmp(data.dmrId, lastData.dmrId) != 0;
  bool targetChanged   = !haveRenderedOnce || strcmp(data.target, lastData.target) != 0;
  bool modeChanged     = !haveRenderedOnce || strcmp(data.mode, lastData.mode) != 0;
  bool durationChanged = !haveRenderedOnce || strcmp(data.duration, lastData.duration) != 0;

  if (!haveRenderedOnce) {
    drawStaticChrome();
  }

  if (callsignChanged) drawCallsignBanner(data, true);
  if (nameChanged)     drawNameLine(data);
  if (dmrChanged)      drawMetaLine(META1_Y, "DMR ID: ", strlen(data.dmrId) > 0 ? data.dmrId : "N/A");
  if (targetChanged)   drawMetaLine(META2_Y, "Target: ", data.target);
  if (modeChanged)     drawMetaLine(META3_Y, "Mode: ", data.mode);
  if (durationChanged) drawMetaLine(META4_Y, "Duration: ", data.duration);

  if (!haveRenderedOnce) {
    updateClockDisplay(true);
  }

  haveRenderedOnce = true;
}

void syncTimeWithNTP() {
  configTime(0, 0, NTP_SERVER);
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
    snprintf(buf, sizeof(buf), "Syncing time with %s...", NTP_SERVER);
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
  drawMetaLine(META1_Y, "DMR ID: ", strlen(lastData.dmrId) > 0 ? lastData.dmrId : "N/A");
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
void getSunRiseSetTimes(int year, int month, int day, float lat, float lon, float &sunriseUTC, float &sunsetUTC) {
  int d = 367 * year - (7 * (year + (month + 9) / 12)) / 4 + (275 * month) / 9 + day - 730531.5;
  float wgt = 282.9404 + 4.70935E-5 * d;
  float ecc = 0.016709 - 1.151E-9 * d;
  float man = fmod(356.0470 + 0.9856002585 * d, 360.0);
  float obl = 23.4393 - 3.563E-7 * d;
  
  float e_rad = man * M_PI / 180.0;
  float sur = man + (180.0 / M_PI) * ecc * 2.0 * sin(e_rad);
  float sur_rad = sur * M_PI / 180.0;
  
  float x = cos(sur_rad) - ecc;
  float y = sin(sur_rad) * cos(obl * M_PI / 180.0);
  float v = atan2(y, x) * 180.0 / M_PI;
  float lon_sun = fmod(v + wgt, 360.0);
  
  float decl = asin(sin(obl * M_PI / 180.0) * sin(lon_sun * M_PI / 180.0)) * 180.0 / M_PI;
  float l_quad = floor(lon_sun / 90.0) * 90.0;
  float r_quad = floor(v / 90.0) * 90.0;
  float ra = (atan2(cos(obl * M_PI / 180.0) * sin(lon_sun * M_PI / 180.0), cos(lon_sun * M_PI / 180.0)) * 180.0 / M_PI + l_quad - r_quad) / 15.0;
  
  float gmst = fmod(280.1606 + 360.9856237 * d, 360.0) / 15.0;
  float lha_sun = cos(90.833 * M_PI / 180.0) / (cos(lat * M_PI / 180.0) * cos(decl * M_PI / 180.0)) - tan(lat * M_PI / 180.0) * tan(decl * M_PI / 180.0);
  
  if (lha_sun > 1.0 || lha_sun < -1.0) {
    sunriseUTC = 6.0;
    sunsetUTC = 18.0;
    return;
  }
  
  float h_angle = acos(lha_sun) * 180.0 / M_PI / 15.0;
  float ut_noon = ra - lon / 15.0 - gmst;
  ut_noon = fmod(ut_noon + 48.0, 24.0);
  
  sunriseUTC = fmod(ut_noon - h_angle + 24.0, 24.0);
  sunsetUTC = fmod(ut_noon + h_angle + 24.0, 24.0);
}

void checkAutoSunTheme(time_t now) {
  struct tm tinfo;
  gmtime_r(&now, &tinfo);

  float sunriseUTC = 6.0f;
  float sunsetUTC = 18.0f;

  getSunRiseSetTimes(
    tinfo.tm_year + 1900,
    tinfo.tm_mon + 1,
    tinfo.tm_mday,
    OBS_LATITUDE,
    OBS_LONGITUDE,
    sunriseUTC,
    sunsetUTC
  );

  float currentUTC =
    tinfo.tm_hour +
    tinfo.tm_min / 60.0f +
    tinfo.tm_sec / 3600.0f;

  bool shouldBeDark =
    currentUTC < sunriseUTC ||
    currentUTC >= sunsetUTC;

  static bool initializedSun = false;
  static bool lastCalculatedDark = false;

  if (!initializedSun) {
    initializedSun = true;
    lastCalculatedDark = shouldBeDark;
    darkMode = shouldBeDark;
    updateThemeColors();
    return;
  }

  if (shouldBeDark != lastCalculatedDark) {
    lastCalculatedDark = shouldBeDark;
    darkMode = shouldBeDark;
    updateThemeColors();

    if (haveRenderedOnce) {
      redrawAfterThemeChange();
    }
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
    if (millis() - lastThemeToggle > TOGGLE_DEBOUNCE_MS) {
      lastThemeToggle = millis();
      toggleDarkMode();
    }
  }
}

// ---------------------------------------------------------------------------
// Network Request Handling
// ---------------------------------------------------------------------------
CallerData parseWPSDHtml(const char* html) {
  CallerData data = {};

  copyField(data.mode, sizeof(data.mode), "N/A");
  copyField(data.target, sizeof(data.target), "N/A");
  copyField(data.duration, sizeof(data.duration), "N/A");

  char value[128];

  if (extractTagWithClass(
        html, "oc_call", value, sizeof(value))) {
    copyField(data.callsign, sizeof(data.callsign), value);
  }

  if (strlen(data.callsign) == 0 &&
      extractTagWithClass(
        html, "callsign", value, sizeof(value))) {
    copyField(data.callsign, sizeof(data.callsign), value);
  }

  if (strlen(data.callsign) == 0 &&
      extractBetween(
        html,
        "class='oc_call'>",
        "</span>",
        value,
        sizeof(value))) {
    stripTags(
      value,
      data.callsign,
      sizeof(data.callsign)
    );
  }

  if (strlen(data.callsign) == 0 &&
      extractBetween(
        html,
        "class=\"oc_call\">",
        "</span>",
        value,
        sizeof(value))) {
    stripTags(
      value,
      data.callsign,
      sizeof(data.callsign)
    );
  }

  if (extractTagWithClass(
        html, "oc_name", value, sizeof(value)) ||
      extractTagWithClass(
        html, "name", value, sizeof(value))) {
    copyField(data.name, sizeof(data.name), value);
  }

  copyField(
    data.dmrId,
    sizeof(data.dmrId),
    extractDmrId(html, data.callsign)
  );

  extractBetween(
    html,
    "<img src='",
    "'",
    data.flagPath,
    sizeof(data.flagPath)
  );

  if (strlen(data.flagPath) == 0) {
    extractBetween(
      html,
      "<img src=\"",
      "\"",
      data.flagPath,
      sizeof(data.flagPath)
    );
  }

  char* queryPos = strchr(data.flagPath, '?');
  if (queryPos) {
    *queryPos = '\0';
  }

  if (!strstr(data.flagPath, ".png")) {
    data.flagPath[0] = '\0';
  }

  const char* modePos = strstr(html, "Mode:");
  if (!modePos) {
    modePos = strstr(html, "mode:");
  }

  if (modePos) {
    if (extractTagWithClass(
          html,
          "dc_info_def",
          value,
          sizeof(value),
          modePos)) {
      copyField(data.mode, sizeof(data.mode), value);
    }
  }


  const char* targetPos = strstr(html, "Target:");
  if (!targetPos) {
    targetPos = strstr(html, "target:");
  }

  if (targetPos) {
    if (extractTagWithClass(
          html,
          "dc_info_def",
          value,
          sizeof(value),
          targetPos)) {
      copyField(data.target, sizeof(data.target), value);
    }
  }


  const char* durationPos = strstr(html, "TX Duration:");
  if (!durationPos) {
    durationPos = strstr(html, "Duration:");
  }

  if (durationPos) {
    if (extractTagWithClass(
          html,
          "dc_info_def",
          value,
          sizeof(value),
          durationPos)) {
      copyField(data.duration, sizeof(data.duration), value);
    }
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
  http.begin(localClient, SERVER_URL);
  http.addHeader("Connection", "close");
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpResponseCode = http.GET();

  checkTouchToggle();

  if (httpResponseCode == HTTP_CODE_OK) {
    String html;
    html.reserve(4096);
    html = http.getString();
    
    CallerData currentData = parseWPSDHtml(html.c_str());

    if (strcmp(currentData.callsign, lastData.callsign) != 0 ||
        strcmp(currentData.duration, lastData.duration) != 0 ||
        strcmp(currentData.name,     lastData.name)     != 0 ||
        strcmp(currentData.target,   lastData.target)   != 0 ||
        strcmp(currentData.mode, lastData.mode)         != 0 ||
        strcmp(currentData.dmrId, lastData.dmrId)       != 0 ||
        strcmp(currentData.flagPath, lastData.flagPath) != 0 ) {
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
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;

  WiFi.disconnect();
  WiFi.begin(SSID, PASSWORD);
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
  tft.print("Connecting WiFi:");
  tft.println(SSID);

  WiFi.begin(SSID, PASSWORD);
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

  if (currentMillis - lastFetchTime >= INTERVAL) {
    lastFetchTime = currentMillis;
    fetchAndDisplayPage();
    checkTouchToggle();
  }

  if (currentMillis - lastClockUpdate >= CLOCK_UPDATE_INTERVAL) {
    lastClockUpdate = currentMillis;
    updateClockDisplay();
  }

  if (WiFi.status() == WL_CONNECTED && currentMillis - lastNtpSync >= NTP_SYNC_INTERVAL) {
    syncTimeWithNTP();
  }
}
