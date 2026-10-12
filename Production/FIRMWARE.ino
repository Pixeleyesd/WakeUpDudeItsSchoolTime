#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// Pin Definitions
#define TFT_SCLK D9  
#define TFT_MOSI D10 
#define TFT_DC   D4
#define TFT_CS   D5
#define TFT_RST  -1  
#define TFT_BL   -1  
#define BUZZER_PIN D7

// Button Assignments Based On Physical Layout
#define BTN_SELECT D0 // SW1 (Top Left)
#define BTN_BACK   D1 // SW2 (Bot Left)
#define BTN_UP     D2 // SW3 (Top Mid)
#define BTN_DOWN   D3 // SW4 (Bot Mid)
#define BTN_ALARM  D6 // SW5 (Top Right)
#define BTN_SLEEP  D8 // SW6 (Bot Right)

const char* ssid = "Sub 2 Pixeleyesd"; 
const char* ntpServer = "pool.ntp.org";

// Theme Definitions
struct Theme { uint16_t bg; uint16_t fg; uint16_t accent; };
#define COLOR_CYAN   0x07FF
#define COLOR_PINK   0xF81F
#define COLOR_NAVY   0x000F
#define COLOR_YELLOW 0xFFE0

Theme themes[] = {
  {ST77XX_BLACK, ST77XX_WHITE, ST77XX_GREEN}, // 0: Classic Dark
  {ST77XX_BLACK, COLOR_CYAN, COLOR_PINK},     // 1: Cyberpunk
  {COLOR_NAVY, ST77XX_WHITE, COLOR_YELLOW},   // 2: Ocean
  {ST77XX_WHITE, ST77XX_BLACK, ST77XX_BLUE}   // 3: Light Mode
};
#define NUM_THEMES 4
int currentTheme = 0;

class MyST7789 : public Adafruit_ST7789 {
public:
  MyST7789(int8_t cs, int8_t dc, int8_t mosi, int8_t sclk, int8_t rst)
    : Adafruit_ST7789(cs, dc, mosi, sclk, rst) {}
  void setOffsets(uint8_t col, uint8_t row) {
    _colstart = _colstart2 = col;
    _rowstart = _rowstart2 = row;
  }
};
MyST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Preferences prefs;

enum UIState { 
  BOOT_INIT, FIRST_BOOT_TZ, FIRST_BOOT_DST, WIFI_SYNC, PROMPT_MANUAL_TIME, 
  HOME, MENU, SET_TIME_H, SET_TIME_M, ALARM_LIST, EDIT_ALARM_STATE, 
  EDIT_ALARM_H, EDIT_ALARM_M, SETTINGS_MENU, EDIT_TZ, EDIT_DST, EDIT_THEME, RINGING 
};
UIState currentState = BOOT_INIT;

bool isTimeValid = false;
int currentHour = 0, currentMinute = 0;
int menuIndex = 0, selectedAlarm = 0, lastTriggeredMinute = -1;

bool tzConfigured = false;
int gmtOffsetHours = 0;
bool dstActive = false;

#define MAX_ALARMS 5
struct Alarm { bool active; int hour; int minute; };
Alarm alarms[MAX_ALARMS];

unsigned long lastActivityTime = 0;
bool screenAwake = true;
const unsigned long SCREEN_TIMEOUT_MS = 120000;

// Debouncing for all 6 buttons
struct Button { uint8_t pin; bool state; bool lastState; unsigned long lastDebounceTime; };
Button btns[6] = {
  {BTN_SELECT, 1, 1, 0}, 
  {BTN_BACK, 1, 1, 0}, 
  {BTN_UP, 1, 1, 0}, 
  {BTN_DOWN, 1, 1, 0},
  {BTN_ALARM, 1, 1, 0},
  {BTN_SLEEP, 1, 1, 0}
};

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  for(int i=0; i<6; i++) pinMode(btns[i].pin, INPUT_PULLUP);

  tft.init(76, 284);
  tft.setOffsets(82, 18);
  tft.invertDisplay(false);
  tft.setRotation(1);
  
  prefs.begin("blare", false);
  tzConfigured = prefs.getBool("tz_conf", false);
  gmtOffsetHours = prefs.getInt("gmt_off", 0);
  dstActive = prefs.getBool("dst_on", false);
  currentTheme = prefs.getInt("theme", 0);
  
  for(int i=0; i<MAX_ALARMS; i++) {
    alarms[i].active = prefs.getBool(("a"+String(i)).c_str(), false);
    alarms[i].hour = prefs.getInt(("h"+String(i)).c_str(), 0);
    alarms[i].minute = prefs.getInt(("m"+String(i)).c_str(), 0);
  }

  tft.fillScreen(themes[currentTheme].bg);

  if(!tzConfigured) currentState = FIRST_BOOT_TZ;
  else currentState = WIFI_SYNC;
  
  lastActivityTime = millis();
}

bool readButton(int index) {
  bool reading = digitalRead(btns[index].pin);
  if (reading != btns[index].lastState) btns[index].lastDebounceTime = millis();
  if ((millis() - btns[index].lastDebounceTime) > 50) {
    if (reading != btns[index].state) {
      btns[index].state = reading;
      if (btns[index].state == LOW) { btns[index].lastState = reading; return true; }
    }
  }
  btns[index].lastState = reading;
  return false;
}

void saveAlarm(int index) {
  prefs.putBool(("a"+String(index)).c_str(), alarms[index].active);
  prefs.putInt(("h"+String(index)).c_str(), alarms[index].hour);
  prefs.putInt(("m"+String(index)).c_str(), alarms[index].minute);
}

void saveSettings() {
  prefs.putBool("tz_conf", true);
  prefs.putInt("gmt_off", gmtOffsetHours);
  prefs.putBool("dst_on", dstActive);
  prefs.putInt("theme", currentTheme);
  configTime(gmtOffsetHours * 3600, dstActive ? 3600 : 0, ntpServer);
}

void attemptWiFiSync() {
  WiFi.begin(ssid);
  unsigned long startAttempt = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) { delay(100); }
  
  if(WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffsetHours * 3600, dstActive ? 3600 : 0, ntpServer);
    struct tm timeinfo;
    if(getLocalTime(&timeinfo, 5000)) isTimeValid = true;
    WiFi.disconnect(true);
    currentState = HOME;
  } else {
    WiFi.disconnect(true);
    currentState = PROMPT_MANUAL_TIME;
  }
}

void updateLocalTimeTracker() {
  if (isTimeValid) {
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)) {
      currentHour = timeinfo.tm_hour;
      currentMinute = timeinfo.tm_min;
    }
  }
}

void sleepScreen(bool sleep) {
  if (sleep) {
    tft.writeCommand(0x10); 
    delay(5);
    tft.enableDisplay(false);
  } else {
    tft.writeCommand(0x11); 
    delay(120);
    tft.enableDisplay(true);
  }
  screenAwake = !sleep;
}

void drawScreen() {
  if(!screenAwake) return;
  tft.fillScreen(themes[currentTheme].bg);
  tft.setCursor(5, 5);
  
  switch(currentState) {
    case FIRST_BOOT_TZ:
    case EDIT_TZ:
      tft.setTextSize(3); tft.setTextColor(themes[currentTheme].fg); tft.println("GMT Offset:");
      tft.setTextSize(4); tft.setTextColor(themes[currentTheme].accent);
      tft.print(gmtOffsetHours > 0 ? "+" : ""); tft.print(gmtOffsetHours);
      break;
    case FIRST_BOOT_DST:
    case EDIT_DST:
      tft.setTextSize(3); tft.setTextColor(themes[currentTheme].fg); tft.println("DST (+1 Hr):");
      tft.setTextSize(4); tft.setTextColor(themes[currentTheme].accent);
      tft.print(dstActive ? "ON" : "OFF");
      break;
    case EDIT_THEME:
      tft.setTextSize(3); tft.setTextColor(themes[currentTheme].fg); tft.println("Theme:");
      tft.setTextSize(3); tft.setTextColor(themes[currentTheme].accent);
      if(currentTheme == 0) tft.print("Classic Dark");
      else if(currentTheme == 1) tft.print("Cyberpunk");
      else if(currentTheme == 2) tft.print("Ocean");
      else tft.print("Light Mode");
      break;
    case WIFI_SYNC:
      tft.setTextSize(2); tft.setTextColor(themes[currentTheme].fg); tft.print("Connecting to\nWiFi...");
      break;
    case PROMPT_MANUAL_TIME:
      tft.setTextSize(2); tft.setTextColor(ST77XX_RED); tft.println("WiFi Failed.");
      tft.setTextColor(themes[currentTheme].fg); tft.println("\nPress Select\nto set time");
      break;
    case HOME:
      tft.setTextSize(6);
      if(isTimeValid) {
        tft.setTextColor(themes[currentTheme].fg);
        char timeStr[6]; sprintf(timeStr, "%02d:%02d", currentHour, currentMinute);
        tft.print(timeStr);
      } else {
        tft.setTextSize(3); tft.setTextColor(themes[currentTheme].fg); tft.print("NO TIME");
      }
      break;
    case MENU:
      tft.setTextSize(3);
      tft.setTextColor(menuIndex == 0 ? themes[currentTheme].accent : themes[currentTheme].fg); tft.println("1. Time");
      tft.setTextColor(menuIndex == 1 ? themes[currentTheme].accent : themes[currentTheme].fg); tft.println("2. Alarms");
      tft.setTextColor(menuIndex == 2 ? themes[currentTheme].accent : themes[currentTheme].fg); tft.println("3. Settings");
      break;
    case SETTINGS_MENU:
      tft.setTextSize(3);
      tft.setTextColor(menuIndex == 0 ? themes[currentTheme].accent : themes[currentTheme].fg); tft.println("1. Timezone");
      tft.setTextColor(menuIndex == 1 ? themes[currentTheme].accent : themes[currentTheme].fg); tft.println("2. DST Toggle");
      tft.setTextColor(menuIndex == 2 ? themes[currentTheme].accent : themes[currentTheme].fg); tft.println("3. Theme");
      break;
    case SET_TIME_H:
    case SET_TIME_M:
      tft.setTextSize(4);
      tft.setTextColor(currentState == SET_TIME_H ? themes[currentTheme].accent : themes[currentTheme].fg); tft.print(currentHour); tft.print(":");
      tft.setTextColor(currentState == SET_TIME_M ? themes[currentTheme].accent : themes[currentTheme].fg); tft.print(currentMinute);
      break;
    case ALARM_LIST:
      tft.setTextSize(2);
      for(int i=0; i<MAX_ALARMS; i++) {
        tft.setTextColor(selectedAlarm == i ? themes[currentTheme].accent : themes[currentTheme].fg);
        tft.print(i+1); tft.print(": ");
        if(alarms[i].active) {
          char buf[10]; sprintf(buf, "%02d:%02d ON", alarms[i].hour, alarms[i].minute); tft.println(buf);
        } else tft.println("OFF");
      }
      break;
    case EDIT_ALARM_STATE:
      tft.setTextSize(3); tft.setTextColor(themes[currentTheme].fg); tft.print("Status: ");
      tft.setTextColor(themes[currentTheme].accent); tft.println(alarms[selectedAlarm].active ? "ON" : "OFF");
      break;
    case EDIT_ALARM_H:
    case EDIT_ALARM_M:
      tft.setTextSize(4);
      tft.setTextColor(currentState == EDIT_ALARM_H ? themes[currentTheme].accent : themes[currentTheme].fg); tft.print(alarms[selectedAlarm].hour); tft.print(":");
      tft.setTextColor(currentState == EDIT_ALARM_M ? themes[currentTheme].accent : themes[currentTheme].fg); tft.print(alarms[selectedAlarm].minute);
      break;
    case RINGING:
      tft.setTextSize(5); tft.setTextColor(ST77XX_RED); tft.print("WAKE UP!");
      break;
  }
}

void loop() {
  if (currentState == WIFI_SYNC) {
    drawScreen();
    attemptWiFiSync();
    drawScreen();
    return;
  }

  bool btnSelect = readButton(0);
  bool btnBack   = readButton(1);
  bool btnUp     = readButton(2);
  bool btnDown   = readButton(3);
  bool btnAlarm  = readButton(4);
  bool btnSleep  = readButton(5);
  
  bool anyButtonPressed = (btnSelect || btnBack || btnUp || btnDown || btnAlarm || btnSleep);
  bool redraw = false;

  updateLocalTimeTracker();

  if (anyButtonPressed) {
    lastActivityTime = millis();
    if (!screenAwake) {
      sleepScreen(false);
      redraw = true;
      return; 
    }
  }

  if (screenAwake && currentState != RINGING && (millis() - lastActivityTime > SCREEN_TIMEOUT_MS)) {
    sleepScreen(true);
  }

  if (currentState != RINGING && isTimeValid) {
    for(int i=0; i<MAX_ALARMS; i++) {
      if(alarms[i].active && alarms[i].hour == currentHour && alarms[i].minute == currentMinute) {
        if(lastTriggeredMinute != currentMinute) {
          currentState = RINGING;
          lastTriggeredMinute = currentMinute;
          if(!screenAwake) sleepScreen(false);
          redraw = true;
        }
      }
    }
  }

  if(currentState == RINGING) {
    if ((millis() / 200) % 2 == 0) tone(BUZZER_PIN, 2500); else tone(BUZZER_PIN, 3500);
    if(anyButtonPressed) { 
      noTone(BUZZER_PIN); 
      currentState = HOME; 
      redraw = true; 
    }
    return; 
  }

  if (anyButtonPressed) redraw = true;

  switch(currentState) {
    case FIRST_BOOT_TZ:
    case EDIT_TZ:
      if(btnUp) gmtOffsetHours = (gmtOffsetHours >= 14) ? -12 : gmtOffsetHours + 1;
      if(btnDown) gmtOffsetHours = (gmtOffsetHours <= -12) ? 14 : gmtOffsetHours - 1;
      if(btnBack && currentState == EDIT_TZ) currentState = SETTINGS_MENU;
      if(btnSelect) {
        if(currentState == FIRST_BOOT_TZ) currentState = FIRST_BOOT_DST;
        else { saveSettings(); currentState = SETTINGS_MENU; }
      }
      break;
    case FIRST_BOOT_DST:
    case EDIT_DST:
      if(btnUp || btnDown) dstActive = !dstActive;
      if(btnBack && currentState == EDIT_DST) currentState = SETTINGS_MENU;
      if(btnBack && currentState == FIRST_BOOT_DST) currentState = FIRST_BOOT_TZ;
      if(btnSelect) {
        saveSettings();
        if(currentState == FIRST_BOOT_DST) currentState = WIFI_SYNC;
        else currentState = SETTINGS_MENU;
      }
      break;
    case EDIT_THEME:
      if(btnUp) currentTheme = (currentTheme + 1) % NUM_THEMES;
      if(btnDown) currentTheme = (currentTheme - 1 < 0) ? NUM_THEMES - 1 : currentTheme - 1;
      if(btnBack) currentState = SETTINGS_MENU;
      if(btnSelect) { saveSettings(); currentState = SETTINGS_MENU; }
      break;
    case PROMPT_MANUAL_TIME:
      if(btnSelect) currentState = SET_TIME_H;
      break;
    case HOME:
      if(btnSelect) { currentState = MENU; menuIndex = 0; }
      if(btnAlarm)  { currentState = ALARM_LIST; selectedAlarm = 0; }
      if(btnSleep)  { sleepScreen(true); } 
      break;
    case MENU:
      if(btnUp) menuIndex = (menuIndex - 1 < 0) ? 2 : menuIndex - 1;
      if(btnDown) menuIndex = (menuIndex + 1) % 3;
      if(btnBack) currentState = HOME;
      if(btnSelect) {
        if(menuIndex == 0) currentState = SET_TIME_H;
        else if(menuIndex == 1) { currentState = ALARM_LIST; selectedAlarm = 0; }
        else { currentState = SETTINGS_MENU; menuIndex = 0; }
      }
      break;
    case SETTINGS_MENU:
      if(btnUp) menuIndex = (menuIndex - 1 < 0) ? 2 : menuIndex - 1;
      if(btnDown) menuIndex = (menuIndex + 1) % 3;
      if(btnBack) { currentState = MENU; menuIndex = 2; }
      if(btnSelect) {
        if(menuIndex == 0) currentState = EDIT_TZ;
        else if(menuIndex == 1) currentState = EDIT_DST;
        else currentState = EDIT_THEME;
      }
      break;
    case SET_TIME_H:
      if(btnUp) currentHour = (currentHour + 1) % 24;
      if(btnDown) currentHour = (currentHour - 1 < 0) ? 23 : currentHour - 1;
      if(btnBack) currentState = MENU;
      if(btnSelect) currentState = SET_TIME_M;
      break;
    case SET_TIME_M:
      if(btnUp) currentMinute = (currentMinute + 1) % 60;
      if(btnDown) currentMinute = (currentMinute - 1 < 0) ? 59 : currentMinute - 1;
      if(btnBack) currentState = SET_TIME_H;
      if(btnSelect) {
        struct timeval tv; tv.tv_sec = currentHour * 3600 + currentMinute * 60; 
        settimeofday(&tv, NULL);
        isTimeValid = true; currentState = HOME;
      }
      break;
    case ALARM_LIST:
      if(btnUp) selectedAlarm = (selectedAlarm - 1 < 0) ? MAX_ALARMS - 1 : selectedAlarm - 1;
      if(btnDown) selectedAlarm = (selectedAlarm + 1) % MAX_ALARMS;
      if(btnBack) { currentState = MENU; menuIndex = 1; }
      if(btnSelect) currentState = EDIT_ALARM_STATE;
      break;
    case EDIT_ALARM_STATE:
      if(btnUp || btnDown) alarms[selectedAlarm].active = !alarms[selectedAlarm].active;
      if(btnBack) currentState = ALARM_LIST;
      if(btnSelect) {
        if(alarms[selectedAlarm].active) currentState = EDIT_ALARM_H;
        else { saveAlarm(selectedAlarm); currentState = ALARM_LIST; }
      }
      break;
    case EDIT_ALARM_H:
      if(btnUp) alarms[selectedAlarm].hour = (alarms[selectedAlarm].hour + 1) % 24;
      if(btnDown) alarms[selectedAlarm].hour = (alarms[selectedAlarm].hour - 1 < 0) ? 23 : alarms[selectedAlarm].hour - 1;
      if(btnBack) currentState = EDIT_ALARM_STATE;
      if(btnSelect) currentState = EDIT_ALARM_M;
      break;
    case EDIT_ALARM_M:
      if(btnUp) alarms[selectedAlarm].minute = (alarms[selectedAlarm].minute + 1) % 60;
      if(btnDown) alarms[selectedAlarm].minute = (alarms[selectedAlarm].minute - 1 < 0) ? 59 : alarms[selectedAlarm].minute - 1;
      if(btnBack) currentState = EDIT_ALARM_H;
      if(btnSelect) { saveAlarm(selectedAlarm); currentState = ALARM_LIST; }
      break;
  }

  static unsigned long lastUpdate = 0;
  if(currentState == HOME && screenAwake && millis() - lastUpdate > 1000) { redraw = true; lastUpdate = millis(); }
  if(redraw) drawScreen();
}