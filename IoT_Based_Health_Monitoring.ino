#include <WiFi.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>
#include <Wire.h>
#include <DHT.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"


// ================= WIFI =================
const char* ssid = "OnePlus Nord 4";
const char* password = "Bhukya@0926";

// ================= FIREBASE =================
#define API_KEY "AIzaSyAwNMzmZnt8XZ6GCQA2oeYBXvYFZmCkHVI"
#define DATABASE_URL "medicine-monitoring-syst-5f393-default-rtdb.asia-southeast1.firebasedatabase.app"
#define PRESCRIPTIONS_ROOT "/prescriptions"
String activePrescriptionPath = "";

// ================= TIME =================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;   // IST
const int daylightOffset_sec = 0;

// ================= OLED =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 128
#define OLED_CS   5
#define OLED_DC   2
#define OLED_RST  4

// ================= Sensor pins define ==========
#define DHT_PIN 33
#define DHT_TYPE DHT11

#define MAX_SDA 21
#define MAX_SCL 22
// ================= led pins ===============
#define TRAY1_LED 13
#define TRAY2_LED 12
#define TRAY3_LED 15
// ================= SWITCH =================
#define SWITCH_PIN 14    // switch used for changing the screen
#define BUTTON_PIN 27    // Pill taken button
#define MISSED_BUTTON_PIN 26    // pill not taken button
#define BUZZER_PIN 25          
#define EMERGENCY_BUTTON_PIN 32
// ================= COLORS =================
#define BLACK   0x0000  
#define WHITE   0xFFFF  
#define RED     0xF800  
#define GREEN   0x07E0  
#define BLUE    0x001F   
#define YELLOW  0xFFE0  
#define CYAN    0x07FF  
#define MAGENTA 0xF81F  
#define ORANGE  0xFD20  

Adafruit_SSD1351 display = Adafruit_SSD1351(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &SPI,
  OLED_CS,
  OLED_DC,
  OLED_RST
);

// ================= FIREBASE OBJECTS =================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
// ================= Sensor Objects ===============
DHT dht(DHT_PIN, DHT_TYPE);
MAX30105 particleSensor;

const byte SPO2_BUFFER_SIZE = 100;
uint32_t irBuffer[SPO2_BUFFER_SIZE];
uint32_t redBuffer[SPO2_BUFFER_SIZE];

int32_t spo2Value = 0;
int8_t validSPO2 = 0;
int32_t heartRateValue = 0;
int8_t validHeartRate = 0;

unsigned long lastDhtRead = 0;
const unsigned long dhtInterval = 3000;

unsigned long lastMaxRead = 0;
const unsigned long maxReadInterval = 5000;
// ================= Action staus =================
String actionStatus = "";
unsigned long actionStatusTime = 0;
const unsigned long actionStatusDuration = 3000;
//  ================= sensor values upload to cloud ============
unsigned long lastSensorUpload = 0;
const unsigned long sensorUploadInterval = 15000;   // every 15 sec
// ================= PATIENT DATA =================
String patientName = "Madhu";
String patientAge  = "21";
String patientSex  = "M";
String doctorName  = "Doctor";
String hospitalName = "Hospital";
String diagnosis   = "Diagnosis";
// here above patient data is used when the esp32 is not sync with firebase data, if this shows on screen then we can know that esp is not synced with fire base
// ================= MEDICINE DATA =================
const int MAX_MEDS = 3;
const int MAX_TIMINGS = 3;

struct TimingSlot {
  String timeText;        // e.g. "6:00 PM"
  String instruction;     // e.g. "After Food"
  bool valid;
};

struct MedicineData {
  String name;
  String quantity;
  String type;
  String imageUrl;
  int trayNumber;
  TimingSlot timings[MAX_TIMINGS];
  bool valid;
};

MedicineData meds[MAX_MEDS];

// flattened reminders
struct ReminderEvent {
  String medicineName;
  String instruction;
  String timeText;
  int minutesOfDay;
  bool valid;
};

const int MAX_EVENTS = MAX_MEDS * MAX_TIMINGS;
ReminderEvent events[MAX_EVENTS];
int eventCount = 0;

// ================= FAKE HEALTH DATA =================
int heartRate = 72;
float temperature = 36.6;  // here this fake data is used when there is a problem with esp and cloud, if esp and cloud connected succefully the esp will send actual sensor data
int spo2 = 98;

// ================= SCREEN CYCLING =================
int autoScreen = 0;
unsigned long lastScreenChange = 0;
const unsigned long screenInterval = 2000;

// ================= FIREBASE REFRESH =================
unsigned long lastFirebaseRead = 0;
const unsigned long firebaseInterval = 120000;

// ================= REMINDER WINDOW =================
const int ACTIVE_WINDOW_MIN = 5;   // present medicine shown for 5 minutes
bool medicineTaken = false;
int lastTakenIndex = -1;
// ------------------------------------------------
// WIFI + TIME + FIREBASE
// ------------------------------------------------
void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
}

void setupTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void setupFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase signUp OK");
    signupOK = true;
  } else {
    Serial.printf("Firebase signUp failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

String getDateTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Time Not Synced";
  }

  char buffer[40];
  strftime(buffer, sizeof(buffer), "%a %d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}
// ---------------------- Temp sensor setup -------
void setupDHT11() {
  dht.begin();
  Serial.println("DHT11 initialized");
}
// ----------------- Heartrate and SpO2 sensor setup --------
void setupMAX30102() {
  Wire.begin(MAX_SDA, MAX_SCL);

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX30102 not found");
    return;
  }

  Serial.println("MAX30102 initialized");

  particleSensor.setup(
    60,   // ledBrightness
    4,    // sampleAverage
    2,    // ledMode (Red + IR)
    100,  // sampleRate
    411,  // pulseWidth
    4096  // adcRange
  );
}
// ------------------------------------------------
// HELPERS
// ------------------------------------------------
void clearMedicineData() {
  for (int i = 0; i < MAX_MEDS; i++) {
    meds[i].name = "";
    meds[i].quantity = "";
    meds[i].type = "";
    meds[i].imageUrl = "";
    meds[i].trayNumber = 0;
    meds[i].valid = false;
    for (int j = 0; j < MAX_TIMINGS; j++) {
      meds[i].timings[j].timeText = "";
      meds[i].timings[j].instruction = "";
      meds[i].timings[j].valid = false;
    }
  }
}

bool parseTimeToMinutes(String timeStr, int &minutesOut) {
  timeStr.trim();
  if (timeStr.length() == 0) return false;

  int colonPos = timeStr.indexOf(':');
  int spacePos = timeStr.indexOf(' ');

  if (colonPos < 0 || spacePos < 0) return false;

  int hour = timeStr.substring(0, colonPos).toInt();
  int minute = timeStr.substring(colonPos + 1, spacePos).toInt();
  String ampm = timeStr.substring(spacePos + 1);
  ampm.trim();
  ampm.toUpperCase();

  if (hour < 1 || hour > 12 || minute < 0 || minute > 59) return false;

  if (ampm == "AM") {
    if (hour == 12) hour = 0;
  } else if (ampm == "PM") {
    if (hour != 12) hour += 12;
  } else {
    return false;
  }

  minutesOut = hour * 60 + minute;
  return true;
}

void buildReminderEvents() {
  eventCount = 0;

  for (int i = 0; i < MAX_EVENTS; i++) {
    events[i].medicineName = "";
    events[i].instruction = "";
    events[i].timeText = "";
    events[i].minutesOfDay = -1;
    events[i].valid = false;
  }

  for (int i = 0; i < MAX_MEDS; i++) {
    if (!meds[i].valid) continue;

    for (int j = 0; j < MAX_TIMINGS; j++) {
      if (!meds[i].timings[j].valid) continue;
      if (eventCount >= MAX_EVENTS) break;

      int mins = -1;
      if (parseTimeToMinutes(meds[i].timings[j].timeText, mins)) {
        events[eventCount].medicineName = meds[i].name;
        events[eventCount].instruction = meds[i].timings[j].instruction;
        events[eventCount].timeText = meds[i].timings[j].timeText;
        events[eventCount].minutesOfDay = mins;
        events[eventCount].valid = true;
        eventCount++;
      }
    }
  }

  // sort by time
  for (int i = 0; i < eventCount - 1; i++) {
    for (int j = i + 1; j < eventCount; j++) {
      if (events[j].minutesOfDay < events[i].minutesOfDay) {
        ReminderEvent temp = events[i];
        events[i] = events[j];
        events[j] = temp;
      }
    }
  }
}

void printEventsToSerial() {
  Serial.println("Reminder Events:");
  for (int i = 0; i < eventCount; i++) {
    Serial.print(i);
    Serial.print(" -> ");
    Serial.print(events[i].medicineName);
    Serial.print(" | ");
    Serial.print(events[i].timeText);
    Serial.print(" | ");
    Serial.println(events[i].instruction);
  }
}

void drawHeader() {
  display.setTextColor(CYAN);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print("Patient: ");
  display.println(patientName);

  display.setCursor(2, 12);
  display.println(getDateTimeString());

  display.drawLine(0, 22, 128, 22, BLUE);
}

void drawBox(int x, int y, int w, int h, uint16_t color) {
  display.drawRect(x, y, w, h, color);
}

void drawProgressBar(int x, int y, int w, int h, int value, uint16_t color) {
  display.drawRect(x, y, w, h, WHITE);
  display.fillRect(x + 1, y + 1, w - 2, h - 2, BLACK);

  int fillWidth = map(value, 0, 100, 0, w - 2);
  if (fillWidth < 0) fillWidth = 0;
  if (fillWidth > (w - 2)) fillWidth = w - 2;

  display.fillRect(x + 1, y + 1, fillWidth, h - 2, color);
}

// ----------------- Temp Read --------------------
void readTemperatureSensor() {
  if (millis() - lastDhtRead < dhtInterval) return;
  lastDhtRead = millis();

  float t = dht.readTemperature();

  if (!isnan(t)) {
    temperature = t;
    Serial.print("Temperature: ");
    Serial.println(temperature);
  } else {
    Serial.println("DHT11 read failed");
  }
}
//  ---------------------- Heartrate and SpO2 read -----------
void readHeartRateAndSpo2() {
  if (millis() - lastMaxRead < maxReadInterval) return;
  lastMaxRead = millis();

  Serial.println("Reading MAX30102... keep finger steady");

  for (byte i = 0; i < SPO2_BUFFER_SIZE; i++) {
    while (particleSensor.available() == false) {
      particleSensor.check();
    }

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }

  // If no finger, skip update
  if (irBuffer[SPO2_BUFFER_SIZE - 1] < 50000) {
    Serial.println("No finger detected on MAX30102");
    return;
  }

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    SPO2_BUFFER_SIZE,
    redBuffer,
    &spo2Value,
    &validSPO2,
    &heartRateValue,
    &validHeartRate
  );

  if (validHeartRate && heartRateValue > 0) {
    heartRate = heartRateValue;
  }

  if (validSPO2 && spo2Value > 0) {
    spo2 = spo2Value;
  }

  Serial.print("Heart Rate: ");
  if (validHeartRate) Serial.println(heartRate);
  else Serial.println("Invalid");

  Serial.print("SpO2: ");
  if (validSPO2) Serial.println(spo2);
  else Serial.println("Invalid");
}

// ------------------------------------------------
// save active prescription
// ------------------------------------------------
void updateActivePrescriptionPath() {
  if (Firebase.RTDB.getString(&fbdo, "/active_prescription/id")) {
    String activeId = fbdo.stringData();

    if (activeId.length() > 0) {
      String newPath = String(PRESCRIPTIONS_ROOT) + "/" + activeId;

      if (newPath != activePrescriptionPath) {
        activePrescriptionPath = newPath;
        Serial.println("Active prescription path updated: " + activePrescriptionPath);
      }
    }
  } else {
    Serial.print("Active prescription read skipped, keeping old path. Error: ");
    Serial.println(fbdo.errorReason());
  }
}
// ------------------------------------------------
// FIREBASE READ
// ------------------------------------------------
void readFirebaseData() {
  if (!signupOK) return;
  if (activePrescriptionPath == "") return;
  if (Firebase.RTDB.getString(&fbdo, activePrescriptionPath + "/patient")){
    patientName = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, activePrescriptionPath + "/age")) {
    patientAge = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, activePrescriptionPath + "/sex")) {
    patientSex = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, activePrescriptionPath + "/doctor")) {
    doctorName = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, activePrescriptionPath + "/hospital")) {
    hospitalName = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, activePrescriptionPath + "/diagnosis")) {
    diagnosis = fbdo.stringData();
  }

  clearMedicineData();

  for (int i = 0; i < MAX_MEDS; i++) {
  String base = activePrescriptionPath + "/medicines/" + String(i);

  if (Firebase.RTDB.getString(&fbdo, base + "/name")) {
    meds[i].name = fbdo.stringData();
    meds[i].valid = true;
  }

  if (Firebase.RTDB.getString(&fbdo, base + "/quantity")) {
    meds[i].quantity = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, base + "/type")) {
    meds[i].type = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, base + "/imageUrl")) {
    meds[i].imageUrl = fbdo.stringData();
  }

  if (Firebase.RTDB.getInt(&fbdo, base + "/trayNumber")) {
    meds[i].trayNumber = fbdo.intData();
  }

  for (int j = 0; j < MAX_TIMINGS; j++) {
    String tbase = base + "/timings/" + String(j);

    if (Firebase.RTDB.getString(&fbdo, tbase + "/time")) {
      meds[i].timings[j].timeText = fbdo.stringData();
      meds[i].timings[j].valid = true;
    }

    if (Firebase.RTDB.getString(&fbdo, tbase + "/instruction")) {
      meds[i].timings[j].instruction = fbdo.stringData();
    }
  }
}

  buildReminderEvents();
  printEventsToSerial();
  Serial.println("Firebase updated");
}
void markMedicineTaken(int index) {

  if (index < 0 || index >= eventCount) return;

  String path = activePrescriptionPath + "/logs/" + String(millis());
  bool ok = true;

  ok &= Firebase.RTDB.setString(&fbdo, path + "/medicine", events[index].medicineName);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/time", events[index].timeText);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/instruction", events[index].instruction);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/status", "TAKEN");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
    ok &= Firebase.RTDB.setString(&fbdo, path + "/timestamp", String(buffer));
  }

  if (ok) {
    Serial.println("Medicine marked TAKEN");
    actionStatus = "TAKEN";
    actionStatusTime = millis();
    writeAdherenceLogToFirebase(index, "TAKEN");
  } else {
    Serial.print("TAKEN write failed: ");
    Serial.println(fbdo.errorReason());
  }
}
void writeAdherenceLogToFirebase(int index, String statusValue) {
  if (index < 0 || index >= eventCount) return;

  String path = "/adherence_logs/" + String(millis());

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time not available for adherence log");
    return;
  }

  char dayBuf[10];
  char dateBuf[20];
  char timeBuf[12];
  char timestampBuf[30];

  strftime(dayBuf, sizeof(dayBuf), "%a", &timeinfo);
  strftime(dateBuf, sizeof(dateBuf), "%d %b %Y", &timeinfo);
  strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
  strftime(timestampBuf, sizeof(timestampBuf), "%d-%m-%Y %H:%M:%S", &timeinfo);

  bool ok = true;
  ok &= Firebase.RTDB.setString(&fbdo, path + "/medicine", events[index].medicineName);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/status", statusValue);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/time", events[index].timeText);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/instruction", events[index].instruction);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/day", String(dayBuf));
  ok &= Firebase.RTDB.setString(&fbdo, path + "/date", String(dateBuf));
  ok &= Firebase.RTDB.setString(&fbdo, path + "/timestamp", String(timestampBuf));

  if (ok) {
    Serial.println("Adherence log written: " + statusValue);
  } else {
    Serial.print("Adherence log failed: ");
    Serial.println(fbdo.errorReason());
  }
}
void markMedicineMissed(int index) {

  if (index < 0 || index >= eventCount) return;

  String path = activePrescriptionPath + "/logs/" + String(millis());
  bool ok = true;

  ok &= Firebase.RTDB.setString(&fbdo, path + "/medicine", events[index].medicineName);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/time", events[index].timeText);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/instruction", events[index].instruction);
  ok &= Firebase.RTDB.setString(&fbdo, path + "/status", "MISSED");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeinfo);
    ok &= Firebase.RTDB.setString(&fbdo, path + "/timestamp", String(buffer));
  }

  if (ok) {
    Serial.println("Medicine marked MISSED");
    actionStatus = "MISSED";
    actionStatusTime = millis();
    writeAdherenceLogToFirebase(index, "MISSED");
  } else {
    Serial.print("MISSED write failed: ");
    Serial.println(fbdo.errorReason());
  }
}
// ------------------------------------------------
// REMINDER STATUS
// ------------------------------------------------
void getReminderState(int &prevIndex, int &nowIndex, int &nextIndex) {
  prevIndex = -1;
  nowIndex = -1;
  nextIndex = -1;

  if (eventCount == 0) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    prevIndex = 0;
    nextIndex = 0;
    return;
  }

  int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  // Find current active medicine
  for (int i = 0; i < eventCount; i++) {
    int diff = nowMinutes - events[i].minutesOfDay;
    if (diff >= 0 && diff < ACTIVE_WINDOW_MIN) {
      nowIndex = i;
      break;
    }
  }

  // Previous medicine
  if (nowIndex != -1) {
    prevIndex = nowIndex - 1;
    if (prevIndex < 0) prevIndex = eventCount - 1;

    nextIndex = nowIndex + 1;
    if (nextIndex >= eventCount) nextIndex = 0;
    return;
  }

  // If no current medicine, find last passed event
  for (int i = 0; i < eventCount; i++) {
    if (events[i].minutesOfDay < nowMinutes) {
      prevIndex = i;
    }
  }
  if (prevIndex == -1) prevIndex = eventCount - 1;

  // Find next event
  for (int i = 0; i < eventCount; i++) {
    if (events[i].minutesOfDay > nowMinutes) {
      nextIndex = i;
      break;
    }
  }
  if (nextIndex == -1) nextIndex = 0;
}

// ------------------------------------------------
// SCREENS
// ------------------------------------------------
void showPatientInfoScreen() {
  display.fillScreen(BLACK);
  drawHeader();

  drawBox(6, 30, 116, 86, GREEN);

  display.setTextColor(YELLOW);
  display.setCursor(25, 36);
  display.println("PATIENT INFO");

  display.setTextColor(WHITE);
  display.setCursor(12, 54);
  display.print("Name: ");
  display.println(patientName);

  display.setCursor(12, 68);
  display.print("Age : ");
  display.println(patientAge);

  display.setCursor(12, 82);
  display.print("Sex : ");
  display.println(patientSex);

  display.setCursor(12, 96);
  display.print("Dr  : ");
  display.println(doctorName);
}

void showDashboard() {
  display.fillScreen(BLACK);
  drawHeader();

  // ---------------- HEART RATE BOX ----------------
  drawBox(4, 28, 58, 28, GREEN);
  display.setTextColor(GREEN);
  display.setCursor(8, 33);
  display.print("HR");

  display.setTextColor(WHITE);
  display.setCursor(24, 43);
  display.print(heartRate);
  display.print(" bpm");

  // ---------------- TEMPERATURE BOX ----------------
  drawBox(66, 28, 58, 28, MAGENTA);
  display.setTextColor(MAGENTA);
  display.setCursor(70, 33);
  display.print("TEMP");

  display.setTextColor(WHITE);
  display.setCursor(72, 43);
  display.print(temperature, 1);
  display.print("C");

  // ---------------- SPO2 BOX ----------------
  drawBox(4, 62, 58, 24, CYAN);
  display.setTextColor(CYAN);
  display.setCursor(8, 68);
  display.print("SpO2");

  display.setTextColor(WHITE);
  display.setCursor(28, 76);
  display.print(spo2);
  display.print("%");

  // ---------------- STATUS LOGIC ----------------
  String statusText;
  uint16_t statusColor;
  int barValue;

  if (heartRate > 120 || heartRate < 50 || temperature > 38.0 || spo2 < 92) {
    statusText = "CRITICAL";
    statusColor = RED;
  }
  else if (heartRate > 100 || temperature > 37.5 || spo2 < 95) {
    statusText = "WARNING";
    statusColor = YELLOW;
  }
  else {
    statusText = "NORMAL";
    statusColor = GREEN;
  }

  // ---------------- LEVEL BOX ----------------
  drawBox(66, 62, 58, 24, YELLOW);
  display.setTextColor(YELLOW);
  display.setCursor(70, 68);
  display.print("LEVEL");

  display.setTextColor(WHITE);
  display.setCursor(74, 76);
  display.print(spo2);
  display.print("%");

  // ---------------- PROGRESS BAR ----------------
  barValue = map(spo2, 85, 100, 0, 100);
  if (barValue < 0) barValue = 0;
  if (barValue > 100) barValue = 100;

  display.setTextColor(WHITE);
  display.setCursor(6, 93);
  display.print("Health");

  drawProgressBar(6, 103, 74, 12, barValue, statusColor);

  // ---------------- STATUS BOX ----------------
  drawBox(86, 94, 38, 24, statusColor);
  display.setTextColor(statusColor);

  if (statusText == "CRITICAL") {
    display.setCursor(89, 102);
    display.print("CRIT");
  }
  else if (statusText == "WARNING") {
    display.setCursor(89, 102);
    display.print("WARN");
  }
  else {
    display.setCursor(97, 102);
    display.print("OK");
  }
}
void showPrescriptionScreen() {
  display.fillScreen(BLACK);
  drawHeader();

  drawBox(4, 28, 120, 90, ORANGE);

  display.setTextColor(ORANGE);
  display.setCursor(22, 34);
  display.println("PRESCRIPTION");

  display.setTextColor(WHITE);
  display.setCursor(10, 48);
  display.print("Doctor: ");
  display.println(doctorName);

  display.setCursor(10, 60);
  display.print("Hosp: ");
  display.println(hospitalName);

  display.setCursor(10, 72);
  display.print("Dx: ");
  display.println(diagnosis);

  int lineY = 86;
  for (int i = 0; i < MAX_MEDS; i++) {
    if (!meds[i].valid) continue;
    display.setCursor(10, lineY);
    display.print(String(i + 1) + ". ");
    display.println(meds[i].name);
    lineY += 12;
    if (lineY > 108) break;
  }
}

void showMedicineReminderScreen() {
  display.fillScreen(BLACK);
  drawHeader();

  int prevIndex, nowIndex, nextIndex;
  getReminderState(prevIndex, nowIndex, nextIndex);

  // ---------------- PREVIOUS ----------------
  drawBox(5, 28, 118, 24, CYAN);
  display.setTextColor(CYAN);
  display.setCursor(10, 32);
  display.print("Prev:");

  display.setTextColor(WHITE);
  display.setCursor(10, 42);
  if (prevIndex >= 0) {
    display.print(events[prevIndex].medicineName);
    display.print(" ");
    display.print(events[prevIndex].timeText);
  } else {
    display.print("N/A");
  }

  // ---------------- CURRENT ----------------
  if (nowIndex >= 0) {
    drawBox(5, 56, 118, 34, RED);
    display.setTextColor(RED);
    display.setCursor(10, 60);
    display.print("Now:");

    display.setTextColor(WHITE);
    display.setCursor(10, 70);
    display.print(events[nowIndex].medicineName);

    display.setCursor(10, 80);
    display.print(events[nowIndex].instruction);

    int tray = getTrayNumberForEvent(nowIndex);
    display.setTextColor(YELLOW);
    display.setCursor(80, 80);
    display.print("T:");
    display.print(tray);
  }

  // ---------------- NEXT ----------------
  drawBox(5, 94, 118, 24, GREEN);
  display.setTextColor(GREEN);
  display.setCursor(10, 98);
  display.print("Next:");

  display.setTextColor(WHITE);
  display.setCursor(10, 108);
  if (nextIndex >= 0) {
    display.print(events[nextIndex].medicineName);
    display.print(" ");
    display.print(events[nextIndex].timeText);
  } else {
    display.print("N/A");
  }

  // ---------------- TAKEN / MISSED STATUS ----------------
  if (millis() - actionStatusTime < actionStatusDuration) {
    if (actionStatus == "TAKEN") {
      drawBox(72, 56, 46, 16, GREEN);
      display.setTextColor(GREEN);
      display.setCursor(78, 61);
      display.print("TAKEN");
    }
    else if (actionStatus == "MISSED") {
      drawBox(68, 56, 50, 16, RED);
      display.setTextColor(RED);
      display.setCursor(72, 61);
      display.print("MISSED");
    }
  }
}
// ------------------------------------------------
// Buzzer and tray leds
// ------------------------------------------------
void updateBuzzerAndTrayLED(int nowIndex) {
  static unsigned long lastToggleTime = 0;
  static bool alertState = false;
  const unsigned long alertInterval = 300;

  if (medicineTaken || nowIndex < 0) {
    digitalWrite(BUZZER_PIN, LOW);
    turnOffTrayLEDs();
    alertState = false;
    return;
  }

  if (millis() - lastToggleTime >= alertInterval) {
    lastToggleTime = millis();
    alertState = !alertState;

    digitalWrite(BUZZER_PIN, alertState ? HIGH : LOW);

    int tray = getTrayNumberForEvent(nowIndex);

    turnOffTrayLEDs();
    if (alertState) {
      if (tray == 1) digitalWrite(TRAY1_LED, HIGH);
      else if (tray == 2) digitalWrite(TRAY2_LED, HIGH);
      else if (tray == 3) digitalWrite(TRAY3_LED, HIGH);
    }
  }
}
// ------------------------------------------------
// Emergency Button
// ------------------------------------------------
void sendEmergencyToFirebase() {
  String path = "/emergency_logs/" + String(millis());

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time not available");
    return;
  }

  char dayBuf[10];
  char dateBuf[20];
  char timeBuf[12];

  strftime(dayBuf, sizeof(dayBuf), "%a", &timeinfo);
  strftime(dateBuf, sizeof(dateBuf), "%d %b %Y", &timeinfo);
  strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);

  bool ok = true;
  ok &= Firebase.RTDB.setString(&fbdo, path + "/day", String(dayBuf));
  ok &= Firebase.RTDB.setString(&fbdo, path + "/date", String(dateBuf));
  ok &= Firebase.RTDB.setString(&fbdo, path + "/time", String(timeBuf));
  ok &= Firebase.RTDB.setString(&fbdo, path + "/message", "Emergency button pressed");

  if (ok) {
    Serial.println("Emergency sent to Firebase");
  } else {
    Serial.print("Emergency write failed: ");
    Serial.println(fbdo.errorReason());
  }
}
String getHealthStatus() {
  if (heartRate > 120 || heartRate < 50 || temperature > 38.0 || spo2 < 92) {
    return "CRITICAL";
  }
  else if (heartRate > 100 || temperature > 37.5 || spo2 < 95) {
    return "WARNING";
  }
  else {
    return "NORMAL";
  }
}
void uploadSensorDataToFirebase() {
  if (!signupOK) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (heartRate <= 0 || spo2 <= 0 || isnan(temperature)) {
  Serial.println("Skipping sensor upload due to invalid values");
  return;
}
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time not available for sensor upload");
    return;
  }

  char dayBuf[10];
  char dateBuf[20];
  char timeBuf[12];
  char timestampBuf[30];

  strftime(dayBuf, sizeof(dayBuf), "%a", &timeinfo);
  strftime(dateBuf, sizeof(dateBuf), "%d %b %Y", &timeinfo);
  strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
  strftime(timestampBuf, sizeof(timestampBuf), "%d-%m-%Y %H:%M:%S", &timeinfo);

  String status = getHealthStatus();

  // ---------- latest_health ----------
  bool ok = true;
  ok &= Firebase.RTDB.setInt(&fbdo, "/latest_health/heartRate", heartRate);
  ok &= Firebase.RTDB.setInt(&fbdo, "/latest_health/spo2", spo2);
  ok &= Firebase.RTDB.setFloat(&fbdo, "/latest_health/temperature", temperature);
  ok &= Firebase.RTDB.setString(&fbdo, "/latest_health/status", status);
  ok &= Firebase.RTDB.setString(&fbdo, "/latest_health/day", String(dayBuf));
  ok &= Firebase.RTDB.setString(&fbdo, "/latest_health/date", String(dateBuf));
  ok &= Firebase.RTDB.setString(&fbdo, "/latest_health/time", String(timeBuf));
  ok &= Firebase.RTDB.setString(&fbdo, "/latest_health/timestamp", String(timestampBuf));

  // ---------- sensor_logs ----------
  String logPath = "/sensor_logs/" + String(millis());
  ok &= Firebase.RTDB.setInt(&fbdo, logPath + "/heartRate", heartRate);
  ok &= Firebase.RTDB.setInt(&fbdo, logPath + "/spo2", spo2);
  ok &= Firebase.RTDB.setFloat(&fbdo, logPath + "/temperature", temperature);
  ok &= Firebase.RTDB.setString(&fbdo, logPath + "/status", status);
  ok &= Firebase.RTDB.setString(&fbdo, logPath + "/day", String(dayBuf));
  ok &= Firebase.RTDB.setString(&fbdo, logPath + "/date", String(dateBuf));
  ok &= Firebase.RTDB.setString(&fbdo, logPath + "/time", String(timeBuf));
  ok &= Firebase.RTDB.setString(&fbdo, logPath + "/timestamp", String(timestampBuf));

  if (ok) {
    Serial.println("Sensor data uploaded to Firebase");
  } else {
    Serial.print("Sensor upload failed: ");
    Serial.println(fbdo.errorReason());
  }
}
void turnOffTrayLEDs() {
  digitalWrite(TRAY1_LED, LOW);
  digitalWrite(TRAY2_LED, LOW);
  digitalWrite(TRAY3_LED, LOW);
}

int getTrayNumberForEvent(int eventIndex) {
  if (eventIndex < 0 || eventIndex >= eventCount) return 0;

  for (int i = 0; i < MAX_MEDS; i++) {
    if (meds[i].valid && meds[i].name == events[eventIndex].medicineName) {
      return meds[i].trayNumber;
    }
  }
  return 0;
}

void updateTrayLEDs(int nowIndex) {
  turnOffTrayLEDs();

  if (medicineTaken || nowIndex < 0) return;

  int tray = getTrayNumberForEvent(nowIndex);

  if (tray == 1) digitalWrite(TRAY1_LED, HIGH);
  else if (tray == 2) digitalWrite(TRAY2_LED, HIGH);
  else if (tray == 3) digitalWrite(TRAY3_LED, HIGH);
}
// ------------------------------------------------
// SETUP + LOOP
// ------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(MISSED_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN,LOW);
  pinMode(EMERGENCY_BUTTON_PIN, INPUT_PULLUP);
  pinMode(TRAY1_LED, OUTPUT);
  pinMode(TRAY2_LED, OUTPUT);
  pinMode(TRAY3_LED, OUTPUT);

  digitalWrite(TRAY1_LED, LOW);
  digitalWrite(TRAY2_LED, LOW);
  digitalWrite(TRAY3_LED, LOW);
  display.begin();

  display.fillScreen(BLACK);
  display.setTextColor(GREEN);
  display.setTextSize(2);
  display.setCursor(18, 50);
  display.println("START");
  delay(800);

  connectWiFi();
  delay(2000);
  setupTime();
  setupFirebase();
  setupDHT11();
  setupMAX30102();
  delay(1000);
  updateActivePrescriptionPath();
  readFirebaseData();
  randomSeed(millis());
}

void loop() {
  unsigned long now = millis();

  if (now - lastFirebaseRead >= firebaseInterval) {
    lastFirebaseRead = now;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, skipping Firebase refresh");
    } else {
      updateActivePrescriptionPath();
      readFirebaseData();
    }
  }

  int switchState = digitalRead(SWITCH_PIN);
  int buttonState = digitalRead(BUTTON_PIN);
  int missedButtonState = digitalRead(MISSED_BUTTON_PIN);
  int emergencyButtonState = digitalRead(EMERGENCY_BUTTON_PIN);

  if (emergencyButtonState == LOW) {
    sendEmergencyToFirebase();
    delay(1000);
  }

  // Temperature can be read anytime
  readTemperatureSensor();

  // Upload latest health data to Firebase
  if (now - lastSensorUpload >= sensorUploadInterval) {
    lastSensorUpload = now;
    uploadSensorDataToFirebase();
  }

  if (switchState == LOW) {
    int prevIndex, nowIndex, nextIndex;
    getReminderState(prevIndex, nowIndex, nextIndex);

    // TAKEN button
    if (buttonState == LOW && nowIndex != -1) {
      if (!medicineTaken || lastTakenIndex != nowIndex) {
        markMedicineTaken(nowIndex);
        medicineTaken = true;
        lastTakenIndex = nowIndex;
      }
    }

    // MISSED button
    if (missedButtonState == LOW && nowIndex != -1) {
      if (!medicineTaken || lastTakenIndex != nowIndex) {
        markMedicineMissed(nowIndex);
        medicineTaken = true;
        lastTakenIndex = nowIndex;
      }
    }

    // Reset after reminder window ends
    if (nowIndex == -1) {
      medicineTaken = false;
    }

    // Buzzer + tray LED sync
    updateBuzzerAndTrayLED(nowIndex);
    showMedicineReminderScreen();

  } else {
    // Normal mode -> everything off
    digitalWrite(BUZZER_PIN, LOW);
    turnOffTrayLEDs();

    // Read MAX30102 only outside reminder mode
    readHeartRateAndSpo2();

    if (now - lastScreenChange >= screenInterval) {
      autoScreen++;
      if (autoScreen > 2) autoScreen = 0;
      lastScreenChange = now;
    }

    if (autoScreen == 0) {
      showPatientInfoScreen();
    }
    else if (autoScreen == 1) {
      showDashboard();
    }
    else if (autoScreen == 2) {
      showPrescriptionScreen();
    }
  }

  delay(100);
}