#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <PNGdec.h>

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

// *******************************
// Copyright 2026 - IZ7ZAK - Basilio - All right reserved - Released under AGPL v3.0 
// If you made no changes callsign can be changed on line 225
// *******************************

TFT_eSPI tft = TFT_eSPI();
PNG png;

unsigned long lastFetchTime = 0;
const unsigned long interval = 1500; // Poll every 1.5s

struct CallerData {
  String callsign = "";
  String dmrId = "";    // 6 to 8-digit DMR / Radio ID
  String name = "";
  String mode = "";
  String target = "";
  String duration = "";
  String flagPath = ""; // Path parsed directly from HTML
};

CallerData lastData;

// Buffer to download PNG file into RAM
uint8_t* pngBuffer = NULL;
size_t pngBufferSize = 0;

// String parsing helpers
String stripTags(String text) {
  String clean = "";
  bool inTag = false;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '<') {
      inTag = true;
    } else if (c == '>') {
      inTag = false;
    } else if (!inTag) {
      if (c != '\r' && c != '\n' && c != '\t') clean += c;
    }
  }
  clean.trim();
  return clean;
}

String extractBetween(const String& html, const String& startStr, const String& endStr) {
  int start = html.indexOf(startStr);
  if (start == -1) return "";
  start += startStr.length();
  int end = html.indexOf(endStr, start);
  if (end == -1) return "";
  return html.substring(start, end);
}

String extractTagWithClass(const String& html, const String& className) {
  int pos = 0;
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

// RadioID.net API lookup using /api/users and "radio_id" key
String getDmrIdForCallsign(const String& callsign) {
  if (callsign.length() == 0 || callsign == "LISTENING...") return "";
  
  static String cachedCallsign = "";
  static String cachedDmrId = "";
  if (callsign == cachedCallsign) return cachedDmrId;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  String url = "https://radioid.net/api/users?callsign=" + callsign;
  http.begin(client, url);
  http.setTimeout(2000);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int idIdx = payload.indexOf("\"radio_id\":");
    if (idIdx != -1) {
      int start = idIdx + 11; // length of '"radio_id":'
      while (start < (int)payload.length() && !isDigit(payload[start])) {
        start++;
      }
      String idStr = "";
      while (start < (int)payload.length() && isDigit(payload[start])) {
        idStr += payload[start];
        start++;
      }
      if (idStr.length() >= 6 && idStr.length() <= 8) {
        cachedCallsign = callsign;
        cachedDmrId = idStr;
        http.end();
        return idStr;
      }
    }
  }
  http.end();
  return "";
}

// DMR ID Extractor (Tries HTML first, falls back to API lookup)
String extractDmrId(const String& html, const String& callsign) {
  if (html.length() == 0) return "";

  // 1. Check for WPSD classes ('oc_id', 'dc_id', 'dmr_id')
  const char* idClasses[] = {"oc_id", "dc_id", "dmr_id", "caller_id", "radio_id"};
  for (int c = 0; c < 5; c++) {
    String tagVal = extractTagWithClass(html, idClasses[c]);
    String digits = "";
    for (size_t i = 0; i < tagVal.length(); i++) {
      if (isDigit(tagVal[i])) digits += tagVal[i];
    }
    if (digits.length() >= 6 && digits.length() <= 8) return digits;
  }

  // 2. Query RadioID API using callsign if HTML lacks DMR ID
  return getDmrIdForCallsign(callsign);
}

// PNGdec callback: Draw decoded PNG scanlines to display (Vertically centered)
int pngDrawCallback(PNGDRAW *pDraw) {
  uint16_t usPixels[250];
  png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  
  if (pDraw->y % 5 == 0) {
    uint16_t scaledLine[50];
    int scaledWidth = pDraw->iWidth / 5;
    int scaledHeight = png.getHeight() / 5; // Get total height from the PNG object
    
    for (int i = 0; i < scaledWidth && i < 50; i++) {
      scaledLine[i] = usPixels[i * 5];
    }
    
    int startX = 250;
    // Callsign banner box spans Y = 26 to 86 (Height = 60). Centered vertically:
    int startY = 26 + ((60 - scaledHeight) / 2) + (pDraw->y / 5);
    
    if (startX + scaledWidth <= 320 && startY < 86) {
      tft.pushImage(startX, startY, scaledWidth, 1, scaledLine);
    }
  }

  return 1;
}

// Downloads PNG flag from WPSD
bool fetchPNGImage(const String& relativePath) {
  if (WiFi.status() != WL_CONNECTED || relativePath.length() == 0) return false;

  String fullUrl = String(hostUrl) + relativePath;
  HTTPClient http;
  http.begin(fullUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();
    if (len > 0) {
      if (pngBuffer != NULL) free(pngBuffer);
      pngBuffer = (uint8_t*)malloc(len);
      if (pngBuffer != NULL) {
        WiFiClient* stream = http.getStreamPtr();
        int bytesRead = 0;
        while (http.connected() && (bytesRead < len)) {
          size_t available = stream->available();
          if (available) {
            int c = stream->readBytes(pngBuffer + bytesRead, available);
            bytesRead += c;
          }
          delay(1);
        }
        pngBufferSize = len;
        http.end();
        return true;
      }
    }
  }
  http.end();
  return false;
}

void renderDashboard(const CallerData& data) {
  uint16_t wpsdOrange = tft.color565(226, 140, 25);

  tft.fillScreen(TFT_WHITE);

  // Top Header Bar
  tft.fillRect(0, 0, 320, 24, wpsdOrange);
  tft.setFreeFont(FSS9);
  tft.setTextColor(TFT_BLACK, wpsdOrange);
  tft.setCursor(10, 17);
  tft.print("IZ7ZAK HOTSPOT LIVE CALLER");

  // Callsign Banner Box
  tft.fillRect(0, 26, 320, 60, wpsdOrange);
  
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

  // Draw Flag
  if (data.flagPath.length() > 0) {
    if (fetchPNGImage(data.flagPath)) {
      int rc = png.openRAM(pngBuffer, pngBufferSize, pngDrawCallback);
      if (rc == PNG_SUCCESS) {
        png.decode(NULL, 0);
        png.close();
      }
    }
  }

  // Name Section
  tft.setFreeFont(FSB9);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(10, 110);
  tft.print(data.name.length() > 0 ? data.name : "No Active Transmission");

  // Divider
  tft.drawFastHLine(0, 122, 320, TFT_LIGHTGREY);

  // Metadata Section
  tft.setFreeFont(FSS9);

  // 1. DMR ID
  tft.setCursor(10, 142);
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.print("DMR ID: ");
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.print(data.dmrId.length() > 0 ? data.dmrId : "N/A");

  // 2. Target / Talkgroup
  tft.setCursor(10, 164);
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.print("Target: ");
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.print(data.target);

  // 3. Mode
  tft.setCursor(10, 186);
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.print("Mode: ");
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.print(data.mode);

  // 4. Duration
  tft.setCursor(10, 208);
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.print("Duration: ");
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.print(data.duration);
}

CallerData parseWPSDHtml(const String& html) {
  CallerData data;

  // Callsign
  data.callsign = extractTagWithClass(html, "oc_call");
  if (data.callsign.length() == 0) {
    data.callsign = extractBetween(html, "class='oc_call'>", "</span>");
  }
  data.callsign.trim();

  // Name
  data.name = extractTagWithClass(html, "oc_name");
  if (data.name.length() == 0) {
    data.name = extractBetween(html, "class='oc_name'>", "</span>");
  }
  data.name = stripTags(data.name);

  // DMR ID
  data.dmrId = extractDmrId(html, data.callsign);

  // Flag Path
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

  // Mode
  int modeIdx = html.indexOf("Mode:");
  if (modeIdx == -1) modeIdx = html.indexOf("mode:");
  if (modeIdx != -1) {
    String sub = html.substring(modeIdx);
    data.mode = extractTagWithClass(sub, "dc_info_def");
    if (data.mode.length() == 0) data.mode = extractBetween(sub, "class='dc_info_def'>", "</span>");
    data.mode = stripTags(data.mode);
  }

  // Target
  int targetIdx = html.indexOf("Target:");
  if (targetIdx == -1) targetIdx = html.indexOf("target:");
  if (targetIdx != -1) {
    String sub = html.substring(targetIdx);
    data.target = extractTagWithClass(sub, "dc_info_def");
    if (data.target.length() == 0) data.target = extractBetween(sub, "class='dc_info_def'>", "</span>");
    data.target = stripTags(data.target);
  }

  // Duration
  int durIdx = html.indexOf("TX Duration:");
  if (durIdx == -1) durIdx = html.indexOf("Duration:");
  if (durIdx != -1) {
    String sub = html.substring(durIdx);
    data.duration = extractTagWithClass(sub, "dc_info_def");
    if (data.duration.length() == 0) data.duration = extractBetween(sub, "class='dc_info_def'>", "</span>");
    data.duration = stripTags(data.duration);
  }

  return data;
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(true); 
  tft.setSwapBytes(false);
  tft.fillScreen(TFT_WHITE);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.setFreeFont(FSS9);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(10, 30);
  tft.print("Connecting WiFi...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  tft.fillScreen(TFT_WHITE);
  tft.setCursor(10, 30);
  tft.print("WiFi Connected!");
  delay(1000);
}

void fetchAndDisplayPage() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      String rawHtml = http.getString();
      CallerData currentData = parseWPSDHtml(rawHtml);

      if (currentData.callsign != lastData.callsign || 
          currentData.duration != lastData.duration) {
        lastData = currentData;
        renderDashboard(currentData);
      }
    } else {
      tft.fillScreen(TFT_WHITE);
      tft.setFreeFont(FSS9);
      tft.setCursor(10, 30);
      tft.setTextColor(TFT_RED, TFT_WHITE);
      tft.print("HTTP Error: ");
      tft.print(httpResponseCode);
    }
    http.end();
  } else {
    tft.fillScreen(TFT_WHITE);
    tft.setFreeFont(FSS9);
    tft.setCursor(10, 30);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("WiFi Disconnected");
  }
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastFetchTime >= interval) {
    lastFetchTime = currentMillis;
    fetchAndDisplayPage();
  }
}
