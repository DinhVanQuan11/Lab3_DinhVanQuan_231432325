#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// LAB 3 - DE 2: ESP32 Arduino + FreeRTOS + DHT22 + MQTT
// LEDs: PUMP GPIO25, FAN GPIO26, MIST (PWM) GPIO27.
#define PIN_DHT 15
#define PIN_PUMP 25
#define PIN_FAN 26
#define PIN_MIST 27
#define DHT_KIND DHT22

const char* WIFI_NAME = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* BASE_TOPIC = "lab3/de2/demo-7c29b";
const char* DATA_TOPIC = "lab3/de2/demo-7c29b/data";
const char* STATE_TOPIC = "lab3/de2/demo-7c29b/state";
const char* MODE_TOPIC = "lab3/de2/demo-7c29b/cmd/mode";
const char* PUMP_TOPIC = "lab3/de2/demo-7c29b/cmd/pump";
const char* FAN_TOPIC = "lab3/de2/demo-7c29b/cmd/fan";

// Fan and pump thresholds are chosen for this DEMO, not prescribed by the lab.
const float FAN_ON_TEMP = 30.0f;  // fan ON at >=30 C
const float FAN_OFF_TEMP = 28.0f; // fan OFF at <=28 C
const float PUMP_ON_HUM = 40.0f;  // pump ON below 40% air humidity (demo proxy)
const float PUMP_OFF_HUM = 55.0f; // pump OFF at >=55%

struct SensorSample { float temperature; float humidity; };
QueueHandle_t dataQueue = nullptr;
QueueHandle_t dataAlert = nullptr;
DHT dht(PIN_DHT, DHT_KIND);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// MQTT task and alert task communicate using a short spinlock-protected snapshot.
portMUX_TYPE controlLock = portMUX_INITIALIZER_UNLOCKED;
bool autoMode = true;
bool manualPump = false;
bool manualFan = false;
bool pumpOn = false;
bool fanOn = false;
int mistPercent = 0;

void initMistPwm() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_MIST, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_MIST, 0);
#endif
}

// Called by alert task only.
void setActuators(bool pump, bool fan, int mist) {
  mist = constrain(mist, 0, 100);
  digitalWrite(PIN_PUMP, pump ? HIGH : LOW);
  digitalWrite(PIN_FAN, fan ? HIGH : LOW);
  const uint32_t duty = (mist * 255 + 50) / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MIST, duty);
#else
  ledcWrite(0, duty);
#endif
  portENTER_CRITICAL(&controlLock);
  pumpOn = pump;
  fanOn = fan;
  mistPercent = mist;
  portEXIT_CRITICAL(&controlLock);
}

// MQTT is accessed ONLY from Core 0 (taskTransmit and its MQTT callback).
void publishState() {
  if (!mqtt.connected()) return;
  bool mode, pump, fan;
  int mist;
  portENTER_CRITICAL(&controlLock);
  mode = autoMode; pump = pumpOn; fan = fanOn; mist = mistPercent;
  portEXIT_CRITICAL(&controlLock);
  char out[128];
  snprintf(out, sizeof(out),
           "{\"mode\":\"%s\",\"pump\":%s,\"fan\":%s,\"mist\":%d}",
           mode ? "AUTO" : "MANUAL", pump ? "true" : "false",
           fan ? "true" : "false", mist);
  mqtt.publish(STATE_TOPIC, out, true);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (length == 0 || length >= 16) return;
  char cmd[16] = {0};
  memcpy(cmd, payload, length);
  if (strcmp(topic, MODE_TOPIC) == 0) {
    if (strcmp(cmd, "AUTO") != 0 && strcmp(cmd, "MANUAL") != 0) return;
    portENTER_CRITICAL(&controlLock);
    // Preserve current outputs when first entering MANUAL.
    if (autoMode && strcmp(cmd, "MANUAL") == 0) {
      manualPump = pumpOn;
      manualFan = fanOn;
    }
    autoMode = strcmp(cmd, "AUTO") == 0;
    portEXIT_CRITICAL(&controlLock);
    Serial.printf("[MODE] %s\n", cmd);
  } else if (strcmp(topic, PUMP_TOPIC) == 0 || strcmp(topic, FAN_TOPIC) == 0) {
    if (strcmp(cmd, "ON") != 0 && strcmp(cmd, "OFF") != 0) return;
    bool on = strcmp(cmd, "ON") == 0;
    bool accepted = false;
    portENTER_CRITICAL(&controlLock);
    if (!autoMode) {
      if (strcmp(topic, PUMP_TOPIC) == 0) manualPump = on;
      else manualFan = on;
      accepted = true;
    }
    portEXIT_CRITICAL(&controlLock);
    Serial.printf("[MANUAL] %s %s\n", accepted ? "Accepted" : "Ignored (AUTO mode):", cmd);
  } else return;
  // Alert task applies changed requests on its next iteration.
  publishState();
}

// CORE 1: sensor read every 2.5 seconds; overwrite latest samples.
void taskReadData(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      SensorSample s = {t, h};
      xQueueOverwrite(dataQueue, &s);
      xQueueOverwrite(dataAlert, &s);
      Serial.printf("[DHT22] Temp %.1f C | Humidity %.1f %%\n", t, h);
    } else Serial.println("[DHT22] Read failed");
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(2500));
  }
}

int humidityZone(float h) {
  if (h < 30.0f) return 3;
  if (h < 35.0f) return 2;
  if (h < 40.0f) return 1;
  return 0;
}

// CORE 1: auto/manual pump & fan plus ALWAYS-auto timed mist cycles.
void taskAlert(void*) {
  SensorSample latest = {NAN, NAN};
  bool haveReading = false;
  bool autoPump = false, autoFan = false;
  int lastMistZone = 0, mistPower = 0;
  bool mistActive = false;
  TickType_t mistEnd = 0;
  const int powers[] = {0, 25, 50, 100};
  const int durations[] = {0, 10, 20, 30};

  for (;;) {
    SensorSample s;
    if (xQueueReceive(dataAlert, &s, pdMS_TO_TICKS(100)) == pdTRUE) {
      latest = s;
      haveReading = true;

      // Hysteresis prevents rapid toggling at the thresholds.
      if (s.temperature >= FAN_ON_TEMP) autoFan = true;
      else if (s.temperature <= FAN_OFF_TEMP) autoFan = false;
      if (s.humidity < PUMP_ON_HUM) autoPump = true;
      else if (s.humidity >= PUMP_OFF_HUM) autoPump = false;

      int zone = humidityZone(s.humidity);
      if (zone != lastMistZone) {
        lastMistZone = zone;
        mistPower = powers[zone];
        mistActive = zone != 0;
        if (mistActive) {
          mistEnd = xTaskGetTickCount() + pdMS_TO_TICKS(durations[zone] * 1000);
          Serial.printf("[MIST] %d%% for %d seconds\n", mistPower, durations[zone]);
        } else Serial.println("[MIST] OFF: humidity >= 40%");
      }
    }
    if (mistActive && (int32_t)(xTaskGetTickCount() - mistEnd) >= 0) {
      mistActive = false;
      mistPower = 0;
      Serial.println("[MIST] Cycle finished; wait for humidity zone change");
    }

    bool mode, requestedPump, requestedFan;
    portENTER_CRITICAL(&controlLock);
    mode = autoMode; requestedPump = manualPump; requestedFan = manualFan;
    portEXIT_CRITICAL(&controlLock);
    // No sensor reading => safe OFF in AUTO. MANUAL can still drive pump/fan.
    bool pump = mode ? (haveReading && autoPump) : requestedPump;
    bool fan = mode ? (haveReading && autoFan) : requestedFan;
    setActuators(pump, fan, mistPower);
  }
}

// CORE 0: WiFi, MQTT, publish sensor queue and actuator state.
void taskTransmit(void*) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(256);
  unsigned long lastWifi = 0, lastMqtt = 0, lastState = 0;
  SensorSample s;
  String clientId = "wokwi-de2-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  for (;;) {
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (!lastWifi || now - lastWifi >= 5000) {
        lastWifi = now;
        WiFi.begin(WIFI_NAME, WIFI_PASSWORD, 6);
        Serial.println("[WIFI] Connecting...");
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!mqtt.connected()) {
      if (!lastMqtt || now - lastMqtt >= 3000) {
        lastMqtt = now;
        if (mqtt.connect(clientId.c_str())) {
          mqtt.subscribe(MODE_TOPIC);
          mqtt.subscribe(PUMP_TOPIC);
          mqtt.subscribe(FAN_TOPIC);
          publishState();
          Serial.println("[MQTT] Connected and subscribed");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    mqtt.loop();
    if (xQueueReceive(dataQueue, &s, 0) == pdTRUE) {
      char payload[100];
      snprintf(payload, sizeof(payload),
               "{\"temperature\":%.1f,\"humidity\":%.1f}", s.temperature, s.humidity);
      mqtt.publish(DATA_TOPIC, payload, true);
      Serial.printf("[MQTT] Publish %s\n", payload);
    }
    if (now - lastState >= 500) {
      lastState = now;
      publishState();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  digitalWrite(PIN_FAN, LOW);
  initMistPwm();
  setActuators(false, false, 0);
  dht.begin();
  dataQueue = xQueueCreate(1, sizeof(SensorSample));
  dataAlert = xQueueCreate(1, sizeof(SensorSample));
  if (!dataQueue || !dataAlert) {
    Serial.println("FATAL: queue allocation failed");
    abort();
  }
  xTaskCreatePinnedToCore(taskTransmit, "Task truyen data", 6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskReadData, "Task doc data", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(taskAlert, "Task canh bao", 4096, nullptr, 2, nullptr, 1);
  Serial.println("[RTOS] TX Core0; Read and Alert Core1; default AUTO");
}
void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
