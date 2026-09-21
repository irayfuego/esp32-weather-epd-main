#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include "calendar.h"
#include "config.h"
#include "display_utils.h"
#include "renderer.h"
#include "_locale.h"
#include FONT_HEADER
#ifndef USE_HTTP
  #include <WiFiClientSecure.h>
#endif

#define NUM_MAX_CALENDAR_ENTRIES 4
#define CALENDAR_LINE_GAP        70
#define GCAL_TIMEOUT_MS          8000
#define GCAL_MAX_ATTEMPTS        2    // network retries before falling back to cache
#define CAL_NVS_NS               "gcal"
#define CAL_NVS_KEY              "json"

typedef struct {
  String start;
  String end;
  String summary;
  bool   allDay;
} calendar_event_t;

static calendar_event_t calendar_entries[NUM_MAX_CALENDAR_ENTRIES];
static tm  *timeCurrent;
static bool calendarStale = false; // true when entries come from NVS cache

static byte calcDayOfWeek(int d, int m, int y)
{
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  y -= m < 3;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7; // Sun=0, Mon=1, ..., Sat=6
}

// Convert a Unicode codepoint (>= 0x80) to printable ASCII.
// Returns '\0' to silently skip decorative/invisible chars.
static char cpToAscii(uint32_t cp)
{
  if (cp >= 0x80 && cp <= 0xFF) {
    switch (cp) {
      case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: return 'A';
      case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: return 'a';
      case 0xC7: return 'C'; // Ç
      case 0xE7: return 'c'; // ç
      case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
      case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
      case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
      case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
      case 0xD1: return 'N'; // Ñ
      case 0xF1: return 'n'; // ñ
      case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
      case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
      case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
      case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
      case 0xAB: return '"'; // «
      case 0xBB: return '"'; // »
      case 0xB7: return '.'; // · middle dot (Catalan l·l)
      default:   return '?';
    }
  }
  // General Punctuation (U+2000–U+206F)
  if (cp >= 0x2000 && cp <= 0x206F) {
    switch (cp) {
      case 0x2010: case 0x2011: case 0x2012:
      case 0x2013: case 0x2014: case 0x2015: return '-';
      case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
      case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
      case 0x2026: return '.'; // ellipsis
      case 0x2022: case 0x2023: return '*';
      default: return '\0';
    }
  }
  return '\0'; // skip emoji, Greek, Cyrillic, etc.
}

static String sanitizeText(const String& input)
{
  const uint8_t *p = (const uint8_t *)input.c_str();
  size_t len = input.length();
  String out;
  out.reserve(len);

  for (size_t i = 0; i < len; ) {
    uint8_t b = p[i];
    if (b < 0x80) {
      out += (char)b;
      i += 1;
    } else if ((b & 0xE0) == 0xC0 && i + 1 < len && (p[i+1] & 0xC0) == 0x80) {
      uint32_t cp = ((b & 0x1F) << 6) | (p[i+1] & 0x3F);
      char a = cpToAscii(cp);
      if (a) out += a;
      i += 2;
    } else if ((b & 0xF0) == 0xE0 && i + 2 < len &&
               (p[i+1] & 0xC0) == 0x80 && (p[i+2] & 0xC0) == 0x80) {
      uint32_t cp = ((uint32_t)(b & 0x0F) << 12)
                  | ((uint32_t)(p[i+1] & 0x3F) << 6)
                  |            (p[i+2] & 0x3F);
      char a = cpToAscii(cp);
      if (a) out += a;
      i += 3;
    } else if ((b & 0xF8) == 0xF0 && i + 3 < len) {
      i += 4; // 4-byte (emoji): skip
    } else {
      i += 1; // invalid byte: skip
    }
  }
  return out;
}

void drawCalendarEntries()
{
  const int xPos0 = 80;
  const int yPos0 = 250;
  const char *CONS_WEEKDAY[7] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"};

  if (calendar_entries[0].start.isEmpty()) return;

  // Small stale indicator above the first entry when data comes from cache
  if (calendarStale) {
    display.setFont(&FONT_12pt8b);
    drawString(xPos0 + 5, yPos0 - 22, "(~)", LEFT);
  }

  for (int i = 0; i < NUM_MAX_CALENDAR_ENTRIES; i++) {
    if (calendar_entries[i].start.isEmpty()) break;

    int year, month, day, hour, minute;
    float second;
    hour = 0;
    minute = 0;
    sscanf(calendar_entries[i].start.c_str(), "%d-%d-%dT%d:%d:%f",
           &year, &month, &day, &hour, &minute, &second);

    int weekday = calcDayOfWeek(day, month, year);
    String shortweekday = CONS_WEEKDAY[weekday];
    String datetodraw = (String)day + "/" + (String)month;

    if (month == (timeCurrent->tm_mon + 1) && day == timeCurrent->tm_mday) {
      shortweekday = "HOY";
      display.setFont(&FONT_16pt8b);
      drawString(xPos0, yPos0 + i * CALENDAR_LINE_GAP, shortweekday, RIGHT);
    } else {
      display.setFont(&FONT_16pt8b);
      drawString(xPos0, yPos0 - 13 + i * CALENDAR_LINE_GAP, shortweekday, RIGHT);
      drawString(xPos0, yPos0 + 13 + i * CALENDAR_LINE_GAP, datetodraw, RIGHT);
    }

    if (calendar_entries[i].allDay) {
      display.setFont(&FONT_12pt8b);
      drawString(xPos0 + 120, yPos0 + i * CALENDAR_LINE_GAP, "[DIA]", RIGHT);
    } else {
      String timetodraw = (String)hour + ":" + (minute < 10 ? "0" : "") + (String)minute;
      display.setFont(&FONT_22pt8b);
      drawString(xPos0 + 120, yPos0 + i * CALENDAR_LINE_GAP, timetodraw, RIGHT);
    }

    String eventTitle = sanitizeText(calendar_entries[i].summary);
    display.setFont(&FONT_26pt8b);
    drawString(xPos0 + 140, yPos0 + i * CALENDAR_LINE_GAP, eventTitle.c_str(), LEFT);
  }
}

static void saveCalendarCache(const String& json)
{
  Preferences prefs;
  if (prefs.begin(CAL_NVS_NS, false)) {
    prefs.putString(CAL_NVS_KEY, json);
    prefs.end();
  }
}

static String loadCalendarCache()
{
  Preferences prefs;
  String result;
  if (prefs.begin(CAL_NVS_NS, true)) {
    result = prefs.getString(CAL_NVS_KEY, "");
    prefs.end();
  }
  return result;
}

static void parseCalendarJson(const String& body)
{
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.println("Calendar JSON error: " + String(err.f_str()));
    return;
  }
  int i = 0;
  for (JsonObject ev : doc["events"].as<JsonArray>()) {
    if (i >= NUM_MAX_CALENDAR_ENTRIES) break;
    calendar_entries[i].start   = ev["start"]   | "";
    calendar_entries[i].end     = ev["end"]     | "";
    calendar_entries[i].summary = ev["summary"] | "";
    calendar_entries[i].allDay  = (ev["allDay"]  | 0) != 0;
    i++;
  }
}

#ifdef USE_HTTP
int getGoogleCalendar(WiFiClient &client, tm *timeInfo)
#else
int getGoogleCalendar(WiFiClientSecure &client, tm *timeInfo)
#endif
{
  timeCurrent = timeInfo;
  client.setTimeout(GCAL_TIMEOUT_MS / 1000); // TLS handshake timeout (seconds)

  HTTPClient http;
  http.setTimeout(GCAL_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Accept-Encoding", "identity");

  int httpResponse = -1;
  for (int attempt = 0; attempt < GCAL_MAX_ATTEMPTS; attempt++) {
    if (attempt > 0) {
      Serial.println("Calendar: retrying...");
      http.end();
      delay(1000);
      http.setTimeout(GCAL_TIMEOUT_MS);
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.addHeader("Accept-Encoding", "identity");
    }
    http.begin(client, GCAL_SCRIPT_URL);
    httpResponse = http.GET();
    Serial.println("Calendar HTTP [" + String(attempt + 1) + "]: " + String(httpResponse));
    if (httpResponse == HTTP_CODE_OK) break;
  }

  if (httpResponse == HTTP_CODE_OK) {
    String body = http.getString();
    saveCalendarCache(body);
    parseCalendarJson(body);
    calendarStale = false;
  } else {
    String cached = loadCalendarCache();
    if (!cached.isEmpty()) {
      Serial.println("Calendar: using cached data");
      parseCalendarJson(cached);
      calendarStale = true;
    }
  }

  http.end();
  return httpResponse;
}
