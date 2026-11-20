#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>

// WIFI AND TEXTBEE CONFIGURATION

const char* ssid = "Me";
const char* password = "123456789";

const char* apiKey = "txb_NlmRsOa6IybOUkHxqSYWjzID7BE9VsyQ";
const char* deviceID = "6af2945d1d68a35498732535";
const char* recipient = "+254799067928";

const char* textBeeURL =
  "https://api.textbee.dev/api/v1/gateway/devices/";

// GPS CONFIGURATION - NEO-6M

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD 9600

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

const unsigned long GPS_MAX_AGE = 15000;

// SMS QUEUE
// A hardware-derived ID is generated once at startup from the ESP32's factory-programmed MAC address. It remains the same after reboot.

char unitDeviceID[16] = "FD-UNKNOWN";

struct SMSMessage {
  bool manualEmergency;
  bool locationUpdate;
  bool locationValid;
  char timestamp[32];
  double latitude;
  double longitude;
  double hdop;
  unsigned long satellites;
};

QueueHandle_t smsQueue;

// This remains true when the first emergency SMS was sent without a valid GPS location.

bool waitingForLocationUpdate = false;

// PIN CONFIGURATION

#define SDA_PIN             21
#define SCL_PIN             22

#define EMERGENCY_BUTTON    4
#define CANCEL_BUTTON       18
#define RESET_BUTTON        19

#define BUZZER_PIN          25
#define VIBRATION_PIN       26

#define MPU_POWER_PIN       34

// OLED CONFIGURATION

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

bool oledReady = false;

// POWER DETECTION

#define POWER_THRESHOLD 1200

bool mpuPowerPresent = false;
bool autoTriggerUsed = false;

unsigned long sensorPowerStarted = 0;
unsigned long lastPowerCheck = 0;

const unsigned long POWER_CHECK_INTERVAL = 250;
const unsigned long SENSOR_ONLINE_TIME = 10000;

// BUTTON DEBOUNCING

struct Button {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  unsigned long lastChange;
};

Button emergencyButton = {EMERGENCY_BUTTON, HIGH, HIGH, 0};
Button cancelButton    = {CANCEL_BUTTON, HIGH, HIGH, 0};
Button resetButton     = {RESET_BUTTON, HIGH, HIGH, 0};

const unsigned long DEBOUNCE_MS = 35;

bool buttonPressed(Button &button) {

  bool reading = digitalRead(button.pin);
  unsigned long now = millis();

  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.lastChange = now;
  }

  if ((now - button.lastChange) >= DEBOUNCE_MS &&
      reading != button.stableState) {

    button.stableState = reading;

    if (button.stableState == LOW) {
      return true;
    }
  }

  return false;
}

// SYSTEM STATES

enum SystemState {
  MONITORING,
  FALL_COUNTDOWN,
  EMERGENCY_ACTIVE
};

SystemState systemState = MONITORING;

unsigned long countdownStarted = 0;

const unsigned long COUNTDOWN_DURATION = 5000;

unsigned long lastDisplayUpdate = 0;

const unsigned long DISPLAY_INTERVAL = 200;

// GPS READING

void updateGPS() {

  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

bool gpsLocationAvailable() {

  return gps.location.isValid() &&
         gps.location.age() <= GPS_MAX_AGE;
}

// GPS date/time is supplied in UTC. Convert it to East Africa Time (UTC+3).
bool gpsClockAvailable() {
  return gps.date.isValid() &&
         gps.time.isValid() &&
         gps.time.age() <= GPS_MAX_AGE;
}

bool isLeapYear(int year) {
  return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

String getLocalDateTime() {
  if (!gpsClockAvailable()) {
    return "GPS time unavailable";
  }

  int year = gps.date.year();
  int month = gps.date.month();
  int day = gps.date.day();

  int hour = gps.time.hour() + 3;  // UTC to EAT
  int minute = gps.time.minute();
  int second = gps.time.second();

  if (hour >= 24) {
    hour -= 24;
    day++;

    const int daysInMonth[] = {
      31, (isLeapYear(year) ? 29 : 28), 31, 30, 31, 30,
      31, 31, 30, 31, 30, 31
    };

    if (day > daysInMonth[month - 1]) {
      day = 1;
      month++;

      if (month > 12) {
        month = 1;
        year++;
      }
    }
  }

  char buffer[32];
  snprintf(
    buffer, sizeof(buffer),
    "%02d/%02d/%04d %02d:%02d:%02d",
    day, month, year, hour, minute, second
  );

  return String(buffer);
}

void captureTimestamp(char *destination, size_t destinationSize) {
  String stamp = getLocalDateTime();
  snprintf(destination, destinationSize, "%s", stamp.c_str());
}

// TEXTBEE SMS FUNCTION

bool sendSMS(const String &message) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. SMS waiting.");
    return false;
  }

  WiFiClientSecure secureClient;

  // Prototype only.Use certificate validationbefore deploying this system.

  secureClient.setInsecure();

  HTTPClient http;

  String url = String(textBeeURL) +
               String(deviceID) +
               "/send-sms";

  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  if (!http.begin(secureClient, url)) {
    Serial.println("HTTPS initialization failed.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", apiKey);

  StaticJsonDocument<512> doc;

  JsonArray recipients = doc.createNestedArray("recipients");
  recipients.add(recipient);

  doc["message"] = message;

  String requestBody;
  serializeJson(doc, requestBody);

  Serial.println();
  Serial.println("Sending SMS through TextBee...");

  int httpCode = http.POST(requestBody);

  bool success = (httpCode >= 200 && httpCode < 300);

  if (success) {

    Serial.println("TextBee accepted the SMS request.");

    Serial.print("HTTP code: ");
    Serial.println(httpCode);

    Serial.println(http.getString());

  } else {

    Serial.print("SMS request failed. HTTP code: ");
    Serial.println(httpCode);

    Serial.println(http.getString());
  }

  http.end();

  return success;
}

// CREATE SMS MESSAGE

String createEmergencyMessage(const SMSMessage &sms) {

  String message;

  message = "Device ID: ";
  message += unitDeviceID;
  message += ". Event time (EAT): ";
  message += sms.timestamp;
  message += ". ";

  if (sms.locationUpdate) {

    message =
      "GPS LOCATION UPDATE: A valid GPS position has now been acquired following the earlier emergency alert. ";

  } else if (sms.manualEmergency) {

    message =
      "EMERGENCY ALERT! The user has manually requested emergency assistance. ";

  } else {

    message =
      "EMERGENCY ALERT! A possible fall has been detected. The alert was not cancelled. ";
  }

  if (sms.locationValid) {

    message += "LATEST GPS LOCATION: ";

    message += "Latitude: ";
    message += String(sms.latitude, 6);

    message += ", Longitude: ";
    message += String(sms.longitude, 6);

    message += ". Satellites: ";
    message += String(sms.satellites);

    if (sms.hdop >= 0) {
      message += ". GPS HDOP: ";
      message += String(sms.hdop, 2);
    }

    message += ". Open Google Maps: ";
    message += "https://www.google.com/maps?q=";

    message += String(sms.latitude, 6);
    message += ",";
    message += String(sms.longitude, 6);

  } else {

    message +=
      "GPS LOCATION UNAVAILABLE. The device is still searching for satellites. A follow-up location message will be sent automatically when a valid GPS fix is obtained.";
  }

  return message;
}

// SMS WORKER TASK

void smsWorker(void *parameter) {

  SMSMessage sms;

  while (true) {

    if (xQueueReceive(
          smsQueue,
          &sms,
          portMAX_DELAY
        ) == pdTRUE) {

      String message = createEmergencyMessage(sms);

      Serial.println();

      Serial.println(
        sms.locationUpdate
        ? "GPS LOCATION UPDATE SMS CONTENT:"
        : "EMERGENCY SMS CONTENT:"
      );

      Serial.println(message);

      bool sent = false;

      while (!sent) {

        if (WiFi.status() == WL_CONNECTED) {
          sent = sendSMS(message);
        } else {
          Serial.println("Waiting for WiFi connection...");
        }

        if (!sent) {

          Serial.println("Retrying SMS in 15 seconds.");

          vTaskDelay(pdMS_TO_TICKS(15000));
        }
      }

      Serial.println(
        sms.locationUpdate
        ? "GPS location update accepted by TextBee."
        : "Emergency SMS request accepted by TextBee."
      );
    }
  }
}

// CAPTURE GPS AND QUEUE INITIAL EMERGENCY SMS

void queueEmergencySMS(bool manualEmergency) {

  SMSMessage sms = {};

  sms.manualEmergency = manualEmergency;
  sms.locationUpdate = false;
  captureTimestamp(sms.timestamp, sizeof(sms.timestamp));

  // Capture the latest valid GPS location.
  sms.locationValid = gpsLocationAvailable();

  sms.latitude = 0;
  sms.longitude = 0;
  sms.hdop = -1;
  sms.satellites = 0;

  if (sms.locationValid) {

    sms.latitude = gps.location.lat();
    sms.longitude = gps.location.lng();

    if (gps.hdop.isValid()) {
      sms.hdop = gps.hdop.hdop();
    }

    if (gps.satellites.isValid()) {
      sms.satellites = gps.satellites.value();
    }

    Serial.println();
    Serial.println("GPS LOCATION CAPTURED");

    Serial.print("Latitude: ");
    Serial.println(sms.latitude, 6);

    Serial.print("Longitude: ");
    Serial.println(sms.longitude, 6);

    Serial.print("Satellites: ");
    Serial.println(sms.satellites);

  } else {

    Serial.println();
    Serial.println("WARNING: No recent valid GPS location.");
  }

  if (smsQueue != NULL) {

    if (xQueueSend(smsQueue, &sms, 0) == pdTRUE) {

      // Wait for a later GPS fix only if the initial emergency SMS did not contain coordinates.
      
      waitingForLocationUpdate = !sms.locationValid;

      Serial.println("Emergency SMS added to queue.");

      if (waitingForLocationUpdate) {
        Serial.println(
          "GPS follow-up is pending. Waiting for a valid fix."
        );
      }

    } else {

      Serial.println("ERROR: SMS queue is full.");
    }

  } else {

    Serial.println("ERROR: SMS queue is not available.");
  }
}

// AUTOMATIC FOLLOW-UP GPS LOCATION SMS

void checkForGPSLocationUpdate() {

  // Do nothing if no location update is pending.
  if (!waitingForLocationUpdate) {
    return;
  }

  // Wait until GPS has a valid and recent location.
  if (!gpsLocationAvailable()) {
    return;
  }

  SMSMessage sms = {};

  sms.manualEmergency = false;
  sms.locationUpdate = true;
  sms.locationValid = true;
  captureTimestamp(sms.timestamp, sizeof(sms.timestamp));

  sms.latitude = gps.location.lat();
  sms.longitude = gps.location.lng();

  sms.hdop = gps.hdop.isValid()
             ? gps.hdop.hdop()
             : -1;

  sms.satellites = gps.satellites.isValid()
                   ? gps.satellites.value()
                   : 0;

  if (smsQueue != NULL &&
      xQueueSend(smsQueue, &sms, 0) == pdTRUE) {

    // Clear the pending flag only after the location update has successfully entered the SMS queue.
    
    waitingForLocationUpdate = false;

    Serial.println();
    Serial.println("GPS FIX ACQUIRED AFTER EMERGENCY.");
    Serial.println("Follow-up GPS location SMS added to queue.");

    Serial.print("Latitude: ");
    Serial.println(sms.latitude, 6);

    Serial.print("Longitude: ");
    Serial.println(sms.longitude, 6);

    Serial.print("Satellites: ");
    Serial.println(sms.satellites);

  } else {

    Serial.println(
      "Could not queue GPS update. Will try again."
    );
  }
}

// BUZZER AND VIBRATION

void outputsOff() {

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(VIBRATION_PIN, LOW);
}

void startWarningPattern() {

  unsigned long t = millis();

  bool beepOn = (t % 700) < 180;

  digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);

  bool vibrationOn = (t % 1000) < 180;

  digitalWrite(VIBRATION_PIN, vibrationOn ? HIGH : LOW);
}

void startEmergencyPattern() {

  unsigned long t = millis();

  unsigned long phase = t % 1200;

  bool buzzerOn =
    (phase < 250) ||
    (phase >= 400 && phase < 650);

  digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);

  unsigned long vibrationPhase = t % 1000;

  bool vibrationOn = vibrationPhase < 450;

  digitalWrite(VIBRATION_PIN, vibrationOn ? HIGH : LOW);
}

// MPU6050 POWER DETECTION

void updateSensorStatus() {

  unsigned long now = millis();

  if (now - lastPowerCheck < POWER_CHECK_INTERVAL) {
    return;
  }

  lastPowerCheck = now;

  int sensorReading = analogRead(MPU_POWER_PIN);

  bool previousStatus = mpuPowerPresent;

  mpuPowerPresent = (sensorReading > POWER_THRESHOLD);

  if (mpuPowerPresent && !previousStatus) {

    sensorPowerStarted = now;

    Serial.println("MPU6050 POWER DETECTED");

    Serial.print("ADC reading: ");
    Serial.println(sensorReading);
  }

  if (!mpuPowerPresent && previousStatus) {

    sensorPowerStarted = 0;

    Serial.println("MPU6050 POWER LOST");
  }

  if (!mpuPowerPresent) {
    sensorPowerStarted = 0;
  }
}

// STATE TRANSITIONS

void startCountdown(const String &reason) {

  systemState = FALL_COUNTDOWN;

  countdownStarted = millis();

  Serial.println();
  Serial.println("!!! FALL ALERT !!!");
  Serial.println(reason);

  Serial.println("Confirmation countdown started.");
  Serial.println("Press B2 to cancel within 5 seconds.");
}

void activateEmergency(
  const String &reason,
  bool manualEmergency
) {

  if (systemState == EMERGENCY_ACTIVE) {
    return;
  }

  systemState = EMERGENCY_ACTIVE;

  Serial.println();
  Serial.println("!!! EMERGENCY ACTIVE !!!");
  Serial.println(reason);

  // Send initial emergency SMS immediately.
  queueEmergencySMS(manualEmergency);

  Serial.println("Buzzer and vibration activated.");
  Serial.println("Emergency SMS queued.");
  Serial.println("Press B3 to reset the system.");
}

void cancelCountdown() {

  if (systemState != FALL_COUNTDOWN) {
    return;
  }

  systemState = MONITORING;

  outputsOff();

  Serial.println();
  Serial.println("COUNTDOWN CANCELLED");
  Serial.println("No emergency SMS sent.");
  Serial.println("System returned to monitoring.");
}

void resetSystem() {

  systemState = MONITORING;

  outputsOff();

  if (mpuPowerPresent && !autoTriggerUsed) {
    sensorPowerStarted = millis();
  }

  Serial.println();
  Serial.println("SYSTEM RESET");
  Serial.println("Buzzer stopped.");
  Serial.println("Vibration stopped.");
  Serial.println("System returned to monitoring.");
}

// OLED DISPLAY

void drawStatusPage() {

  if (!oledReady) {
    return;
  }

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("Multi Event Detection");
  display.println(" ");

  display.print("MPU POWER: ");
  display.println(mpuPowerPresent ? "ON" : "OFF");

  display.print("GPS: ");

  if (gpsLocationAvailable()) {

    display.print("FIX ");

    display.print(
      gps.satellites.isValid()
      ? gps.satellites.value()
      : 0
    );

    display.println(" SAT");

  } else {

    display.println("NO FIX");
  }

  display.print("WIFI: ");

  display.println(
    WiFi.status() == WL_CONNECTED
    ? "CONNECTED"
    : "OFFLINE"
  );

  switch (systemState) {

    case MONITORING:

      display.println("STATUS: MONITORING");

      if (!mpuPowerPresent) {

        display.println("Waiting for power...");

      } else if (!autoTriggerUsed) {

        unsigned long elapsed =
          millis() - sensorPowerStarted;

        int secondsLeft =
          10 - (int)(elapsed / 1000);

        if (secondsLeft < 0) {
          secondsLeft = 0;
        }

        display.print("Auto test: ");
        display.print(secondsLeft);
        display.println(" sec");

      } else {

        display.println("System ready");
      }

      break;

    case FALL_COUNTDOWN: {

      unsigned long elapsed =
        millis() - countdownStarted;

      unsigned long remaining =
        elapsed >= COUNTDOWN_DURATION
        ? 0
        : COUNTDOWN_DURATION - elapsed;

      int secondsLeft = (remaining + 999) / 1000;

      display.println("!!! FALL ALERT !!!");

      display.print("Confirm in: ");
      display.print(secondsLeft);
      display.println(" sec");

      display.println("B1: EMERGENCY NOW");
      display.println("B2: CANCEL");
      display.println("B3: RESET");

      break;
    }

    case EMERGENCY_ACTIVE:

      display.println("!!! EMERGENCY !!!");
      display.println("ALARM ACTIVE");
      display.println("Buzzer + Vibration");

      if (waitingForLocationUpdate) {
        display.println("Waiting for GPS...");
      } else {
        display.println("GPS SMS QUEUED");
      }

      display.println("B3: RESET SYSTEM");

      break;
  }

  display.display();
}

// OLED PAGE ROTATION: status, device/date-time, and GPS details.
const unsigned long DISPLAY_PAGE_INTERVAL = 3000;
unsigned long lastPageChange = 0;
uint8_t displayPage = 0;

void drawIdentityTimePage() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("DEVICE IDENTIFICATION");
  display.println();

  display.print("ID: ");
  display.println(unitDeviceID);
  display.println();

  if (gpsClockAvailable()) {
    String localDateTime = getLocalDateTime();

    display.print("DATE: ");
    display.println(localDateTime.substring(0, 10));

    display.print("TIME: ");
    display.println(localDateTime.substring(11));
    display.println("TIME SOURCE: GPS");
  } else {
    display.println("DATE: WAITING GPS");
    display.println("TIME: WAITING GPS");
    display.println("TIME NOT YET VALID");
  }

  display.display();
}

void drawGPSPage() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("GPS INFORMATION");

  if (gpsLocationAvailable()) {
    display.print("SATELLITES: ");
    display.println(
      gps.satellites.isValid() ? gps.satellites.value() : 0
    );

    display.print("LAT: ");
    display.println(gps.location.lat(), 5);

    display.print("LON: ");
    display.println(gps.location.lng(), 5);

    display.print("HDOP: ");
    if (gps.hdop.isValid()) {
      display.println(gps.hdop.hdop(), 1);
    } else {
      display.println("N/A");
    }

    display.println("LOCATION: VALID");
  } else {
    display.println("GPS: SEARCHING");
    display.println("Waiting for satellite");
    display.println("location fix...");
  }

  display.display();
}

void drawDisplay() {
  if (!oledReady) {
    return;
  }

  unsigned long now = millis();

  if (now - lastPageChange >= DISPLAY_PAGE_INTERVAL) {
    lastPageChange = now;
    displayPage = (displayPage + 1) % 3;
  }

  if (displayPage == 0) {
    drawStatusPage();
  } else if (displayPage == 1) {
    drawIdentityTimePage();
  } else {
    drawGPSPage();
  }
}

// WIFI RECONNECTION

unsigned long lastWiFiCheck = 0;

void maintainWiFi() {

  if (millis() - lastWiFiCheck < 10000) {
    return;
  }

  lastWiFiCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("WiFi disconnected. Reconnecting...");

    WiFi.reconnect();
  }
}

// SETUP

void setup() {

  Serial.begin(115200);

  // Derive a stable per-unit identifier from the ESP32 factory MAC.
  uint32_t macSuffix = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  snprintf(unitDeviceID, sizeof(unitDeviceID), "FD-%06lX",
           (unsigned long)macSuffix);

  Serial.print("Unique device ID: ");
  Serial.println(unitDeviceID);

  analogReadResolution(12);

  pinMode(MPU_POWER_PIN, INPUT);

  pinMode(EMERGENCY_BUTTON, INPUT_PULLUP);
  pinMode(CANCEL_BUTTON, INPUT_PULLUP);
  pinMode(RESET_BUTTON, INPUT_PULLUP);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VIBRATION_PIN, OUTPUT);

  outputsOff();

  // GPS INITIALIZATION
  gpsSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX_PIN,
    GPS_TX_PIN
  );

  Serial.println("NEO-6M GPS initialized.");
  Serial.println("GPS date/time will be displayed in EAT (UTC+3).");

  // SMS QUEUE
  smsQueue = xQueueCreate(5, sizeof(SMSMessage));

  if (smsQueue != NULL) {

    BaseType_t taskCreated = xTaskCreate(
      smsWorker,
      "SMS Worker",
      8192,
      NULL,
      1,
      NULL
    );

    if (taskCreated == pdPASS) {
      Serial.println("SMS worker initialized.");
    } else {
      Serial.println("ERROR: SMS worker task creation failed.");
    }

  } else {

    Serial.println("ERROR: SMS queue creation failed.");
  }

  // WIFI INITIALIZATION
  WiFi.mode(WIFI_STA);

  WiFi.setAutoReconnect(true);

  WiFi.begin(ssid, password);

  Serial.println("Connecting to WiFi...");

  // OLED INITIALIZATION
  Wire.begin(SDA_PIN, SCL_PIN);

  Wire.setClock(100000);

  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {

    oledReady = true;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);

    display.println("FALL DETECTION");
    display.println(unitDeviceID);
    display.println();
    display.println("Starting...");
    display.println("GPS Initializing");

    display.display();

  } else {

    Serial.println("OLED not found.");
  }

  Serial.println();
  Serial.println("MULTI EVENT DETECTION");
  Serial.println(" ");

  Serial.println("B1 GPIO4: EMERGENCY");
  Serial.println("B2 GPIO18: CANCEL");
  Serial.println("B3 GPIO19: RESET");

  Serial.println("GPS: NEO-6M");
  Serial.println("GPS RX: GPIO16");
  Serial.println("GPS TX: GPIO17");

  Serial.println("SMS: TextBee over WiFi");

  Serial.println();
  Serial.println("Automatic simulated trigger:");
  Serial.println("10 seconds of continuous MPU power.");

  Serial.println("System initialized.");
}

// MAIN LOOP

void loop() {

  unsigned long now = millis();

  // Continuously decode incoming GPS data.
  updateGPS();

  // Automatically queue a follow-up location SMS when GPS gets a valid fix after an emergency.
  
  checkForGPSLocationUpdate();

  // Maintain WiFi connection.
  maintainWiFi();

  // Update MPU power detection.
  updateSensorStatus();

  // Read buttons independently.
  bool emergencyPressed =
    buttonPressed(emergencyButton);

  bool cancelPressed =
    buttonPressed(cancelButton);

  bool resetPressed =
    buttonPressed(resetButton);

  
  // B1: EMERGENCY ONLY

  if (emergencyPressed) {

    activateEmergency(
      "Manual emergency triggered by B1",
      true
    );
  }

  // B2: CANCEL COUNTDOWN ONLY

  if (cancelPressed) {

    if (systemState == FALL_COUNTDOWN) {

      cancelCountdown();

    } else {

      Serial.println("B2 pressed: No pending countdown.");
    }
  }

  // B3: RESET SYSTEM ONLY

  if (resetPressed) {

    resetSystem();
  }

  // AUTOMATIC SIMULATED ALERT

  if (systemState == MONITORING &&
      mpuPowerPresent &&
      !autoTriggerUsed &&
      sensorPowerStarted != 0 &&
      (now - sensorPowerStarted >= SENSOR_ONLINE_TIME)) {

    autoTriggerUsed = true;

    startCountdown("Automatic simulated trigger");
  }

  // COUNTDOWN OPERATION

  if (systemState == FALL_COUNTDOWN) {

    if (now - countdownStarted >= COUNTDOWN_DURATION) {

      activateEmergency(
        "Fall alert not cancelled",
        false
      );

    } else {

      startWarningPattern();
    }
  }

  // EMERGENCY OPERATION

  if (systemState == EMERGENCY_ACTIVE) {

    startEmergencyPattern();
  }

  // MONITORING OPERATION

  if (systemState == MONITORING) {

    outputsOff();
  }

  // OLED REFRESH

  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {

    lastDisplayUpdate = now;

    drawDisplay();
  }
}