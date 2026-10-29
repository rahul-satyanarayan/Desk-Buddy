#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <RTClib.h>
#include <DHT.h>
#include <Preferences.h>
#include <math.h>

// --- Pin Definitions ---
#define SDA_PIN 8
#define SCL_PIN 9
#define DHT_PIN 3
#define BUZZER_PIN 5
#define BUTTON_PIN 7
#define i2c_Address 0x3c 

Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire, -1);
RTC_DS3231 rtc;
DHT dht(DHT_PIN, DHT22);
Preferences prefs;

enum AppState { FACE_TIME, DATA_DISPLAY, ALARM_CONFIG };
AppState currentState = FACE_TIME;
enum ConfigStep { SET_H, SET_M };
ConfigStep currentStep = SET_H;

// --- USER CONFIGURATION (Timing) ---
unsigned long animationTime    = 10000; 
unsigned long infoTime         = 3500; 
unsigned long alarmTriggerTime = 3000;  // How long to hold for SET ALARM (3s)
unsigned long alarmCooldown    = 2000;  // Ignore button for 2s after stopping buzzer

// --- Global Variables ---
float t = 0, h = 0;
int alH = 7, alM = 0;
bool isRinging = false;
unsigned long lastActionTime = 0; 
unsigned long lastInputTime  = 0;
unsigned long lastSensors    = 0;
unsigned long btnTimer       = 0;

const char* daysOfWeek[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

void setup() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  display.begin(i2c_Address, true);
  rtc.begin();
  dht.begin();
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  prefs.begin("chronos", false);
  alH = prefs.getInt("alH", 7);
  alM = prefs.getInt("alM", 0);
  
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
}

// ---------------------------------------------------------
// ICON PRIMITIVES (🌡️, 💧, ⏰, 📅)
// ---------------------------------------------------------

void drawThermometer(int x, int y) {
  display.drawRoundRect(x+2, y, 4, 10, 2, 1);
  display.fillCircle(x+4, y+10, 3, 1);
  display.drawLine(x+4, y+3, x+4, y+8, 0); 
}

void drawDroplet(int x, int y) {
  display.fillCircle(x, y, 4, 1);
  display.fillTriangle(x-4, y, x+4, y, x, y-8, 1);
}

void drawCalendarIcon(int x, int y) {
  display.drawRect(x, y, 14, 12, 1);
  display.drawLine(x, y+3, x+14, y+3, 1);
  display.drawPixel(x+3, y-1, 1);
  display.drawPixel(x+11, y-1, 1);
}

void drawAlarmIcon(int x, int y, bool vibrating) {
  int offX = vibrating ? (millis() % 6) - 3 : 0;
  int curX = x + offX;
  display.drawCircle(curX, y, 8, 1);
  display.drawLine(curX-6, y-8, curX-9, y-11, 1);
  display.drawLine(curX+6, y-8, curX+9, y-11, 1);
  if(vibrating) {
    display.drawLine(curX-12, y-5, curX-15, y-8, 1);
    display.drawLine(curX+12, y-5, curX+15, y-8, 1);
  }
}

void drawAnalogClock(int h, int m, int s) {
  int cx = 64, cy = 32, r = 26;
  display.drawCircle(cx, cy, r, 1);
  float mAngle = (m * 6) * M_PI / 180.0 - M_PI / 2.0;
  display.drawLine(cx, cy, cx + cos(mAngle)*20, cy + sin(mAngle)*20, 1);
  float hAngle = (h * 30 + m / 2.0) * M_PI / 180.0 - M_PI / 2.0;
  display.drawLine(cx, cy, cx + cos(hAngle)*14, cy + sin(hAngle)*14, 1);
}

// ---------------------------------------------------------
// REPAIRED BUTTON HANDLER (Precision Timing)
// ---------------------------------------------------------

void handleButton() {
  static bool lastBtnState = HIGH;
  bool currentBtnState = digitalRead(BUTTON_PIN);

  if (lastBtnState == HIGH && currentBtnState == LOW) {
    btnTimer = millis();
    if (isRinging) {
      isRinging = false;
      digitalWrite(BUZZER_PIN, LOW);
      lastActionTime = millis(); // Cooldown starts
    }
  }

  if (lastBtnState == LOW && currentBtnState == HIGH) {
    unsigned long duration = millis() - btnTimer;

    // 1. Check Cooldown
    if (millis() - lastActionTime < alarmCooldown) {
      return; 
    }

    // 2. Process based on duration vs alarmTriggerTime
    if (duration >= alarmTriggerTime) { 
      currentState = ALARM_CONFIG;
      currentStep = SET_H;
      lastInputTime = millis();
    } 
    else if (duration > 50 && currentState == ALARM_CONFIG) {
      if (currentStep == SET_H) alH = (alH + 1) % 24; 
      else alM = (alM + 1) % 60;
      lastInputTime = millis();
    }
  }
  lastBtnState = currentBtnState;
}

// ---------------------------------------------------------
// DATA OVERLAY (6 Screen Cycle)
// ---------------------------------------------------------

void drawDataOverlay() {
  DateTime now = rtc.now();
  unsigned long timer = (millis() % (infoTime * 6));
  display.setTextSize(2);

  if (timer < infoTime) { // 1. DIGITAL
    display.setCursor(15, 25); display.printf("%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  } else if (timer < infoTime * 2) { // 2. ANALOG
    drawAnalogClock(now.hour(), now.minute(), now.second());
  } else if (timer < infoTime * 3) { // 3. DATE
    drawCalendarIcon(12, 25); display.setCursor(32, 15); display.print(daysOfWeek[now.dayOfTheWeek()]); 
    display.setCursor(32, 40); display.printf("%02d/%02d", now.day(), now.month());
  } else if (timer < infoTime * 4) { // 4. TEMP
    drawThermometer(20, 20); display.setCursor(45, 25); display.printf("%.1f C", t); 
  } else if (timer < infoTime * 5) { // 5. HUMIDITY
    drawDroplet(25, 35); display.setCursor(45, 25); display.printf("%.0f %%", h);
  } else { // 6. ALARM STATUS
    drawAlarmIcon(25, 32, false); display.setCursor(45, 25); display.printf("%02d:%02d", alH, alM);
  }
}

// ---------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------

void loop() {
  handleButton();
  DateTime now = rtc.now();
  if (now.hour() == alH && now.minute() == alM && now.second() == 0) isRinging = true;

  if (isRinging) {
    digitalWrite(BUZZER_PIN, (millis() / 200) % 2);
    display.clearDisplay();
    drawAlarmIcon(64, 32, true); // Vibrating Alarm ⏰
    display.display();
    return;
  }

  display.clearDisplay();
  if (currentState == ALARM_CONFIG) {
    drawAlarmIcon(10, 10, false);
    display.setTextSize(1); display.setCursor(28, 5); 
    display.print(currentStep == SET_H ? "SETTING: HOURS" : "SETTING: MINUTES");
    display.setTextSize(3); display.setCursor(20, 25); display.printf("%02d:%02d", alH, alM);
    
    // Auto-advance logic (2 seconds idle)
    if (millis() - lastInputTime > 2000) {
      if (currentStep == SET_H) { currentStep = SET_M; lastInputTime = millis(); }
      else { prefs.putInt("alH", alH); prefs.putInt("alM", alM); currentState = FACE_TIME; }
    }
  } 
  else {
    unsigned long totalCycle = animationTime + (infoTime * 6);
    unsigned long timer = millis() % totalCycle; 
    
    if (timer < animationTime) {
      // Buddy Expressions (Idle, Cat, Wink)
      int fm = (millis() / 5000) % 3;
      int lx=20, rx=76, ey=10;
      if(fm==0){ display.fillRoundRect(lx,ey,32,40,10,1); display.fillRoundRect(rx,ey,32,40,10,1); display.fillCircle(lx+16,ey+20,7,0); display.fillCircle(rx+16,ey+20,7,0); }
      else if(fm==1){ display.fillRoundRect(lx-4,ey+12,40,18,8,1); display.fillRoundRect(rx-4,ey+12,40,18,8,1); display.fillCircle(lx+16,ey+21,5,0); display.fillCircle(rx+16,ey+21,5,0); }
      else { display.fillRoundRect(lx,ey,32,40,10,1); display.fillCircle(lx+16,ey+20,7,0); for(int i=0;i<3;i++) display.drawLine(rx,ey+15+i,rx+25,ey+23+i,1); }
      display.drawCircle(64, 47, 8, 1); display.fillRect(54, 37, 20, 10, 0); // Mouth
    } else {
      drawDataOverlay();
    }
  }

  if (millis() - lastSensors > 2000) { t = dht.readTemperature(); h = dht.readHumidity(); lastSensors = millis(); }
  display.display();
}