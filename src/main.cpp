#include <WiFiClientSecure.h>
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <BH1750.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "credentials.h"


// --- MQTT Broker details ---
const char* mqtt_server = "abf09ccff5484da78e04fb5444f1faf9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;   // Secure port for MQTT over TLS
const char* mqtt_client_id = "ESP32_Greenhouse_Client_RTOS";

// --- MQTT Topics ---
const char* topic_telemetry = "greenhouse/1/telemetry";
const char* topic_commands = "greenhouse/1/commands";

// --- Define GPIO pins ---
const uint8_t pin_pump = D2;  
const uint8_t pin_servo = D6;
const uint8_t pin_fan_intake = D4;
const uint8_t pin_fan_exhaust = D3;
const uint8_t pin_dht22 = D5;
const uint8_t soil_sensors_pin[] = {A0,A1,A2,A3};
const uint8_t pin_led_r = D8;
const uint8_t pin_led_g = D9; 
const uint8_t pin_led_b = D7;

// --- Window servo positions ---
const int window_close = 104;
const int window_open = 30;  

// --- Operation modes ---
enum OperatingMode {AUTO, MANUAL};
OperatingMode currentMode = MANUAL;  // Default mode 

// --- Detailed crop profiles ---
struct LightRecipe{
  int r,g,b;
};
struct PlantProfile {
  const char* name;
  float temp_max;
  float temp_min;
  int humidity_max;
  int soil_moisture_target;
  float light_lux_target;
  int hours_of_light;
  LightRecipe germination_light;
  LightRecipe growth_light;
  LightRecipe flowering_light;
};

// --- Data base of plant profiles ---
PlantProfile lettuceProfile = {"lettuce", 22.0, 15.0, 80, 65, 12000, 14, {100,100,255}, {255,100,200}, {255,150,150}};
PlantProfile tomatoProfile = {"tomato", 26.0, 18.0, 70, 60, 20000, 16, {120,120,255}, {255,80,180}, {255,200,100}};

PlantProfile* currentProfile = &lettuceProfile; // Puntero al perfil actual
LightRecipe* currentLightRecipe = &lettuceProfile.growth_light; // Puntero al perfil de luz actual

// --- Data structures for sensor data and actuator states ---
struct SensorData {
  float temperature;
  float humidity;
  float lux;
  float ppfd;
  int soil_moisture[4];
  float soil_avg;
};

struct ActuatorState {
  int pump;         // 0 or 1
  int fan_intake;
  int fan_exhaust;
  int window;
  LightRecipe led_rgb;
};

// --- Current manual state ---
ActuatorState currentManualState = {0, 0, 0, window_close, {0, 0, 0}};

// --- Global objects ---
Servo servo;
BH1750 lightSensor; // Sensor connected over I2C
DHT dht(pin_dht22, DHT22);
WiFiClientSecure espClient;   
PubSubClient client(espClient);

// --- Task handlers ---
QueueHandle_t sensorDataQueue;
QueueHandle_t actuatorCommandQueue;
QueueHandle_t telemetryQueue;
SemaphoreHandle_t i2cMutex;

// --- Declare task and functions ---
void TaskSensors(void *pvParameters);
void TaskControl(void *pvParameters);
void TaskActuators(void *pvParameters); 
void TaskComm(void *pvParameters);
void callback(char* topic, byte* message, unsigned int lenght);

// lux to ppfd conversion
float luxToPPFD(float lux) {
  const float conversionFactor = 0.0185; // This factor can vary based on light source
  return lux * conversionFactor;
}

// --- Initialize and validate DHT22 sensor ---
bool initDHT() {
  dht.begin(); 
  for (uint8_t attempt = 1; attempt <= 5; attempt++) {
    delay(2000);
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    Serial.print("Temp: "); Serial.print(t); Serial.print(" °C ");
    Serial.print("Humidity: "); Serial.print(h); Serial.println(" %");
    if (!isnan(t) && !isnan(h)) {
      return true;
    }
  }
  return false;
}

unsigned long lastMsg = 0;
const int msg_interval = 10000;

// --- Function to connect to WiFi ---
void setup_wifi() {
  delay(10);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected!");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize NTP for time synchronization 
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.println(asctime(&timeinfo));
}

// --- MQTT callback function ---
void callback(char* topic, byte* message, unsigned int lenght){
  
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;

  for (int i = 0; i < lenght; i++) {
    Serial.print((char)message[i]);  
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Convert the message to a JSON document
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, message, lenght);
  if (error) {
    Serial.print("Failed to parse JSON: ");
    Serial.println(error.c_str());
    return;
  }

  // Parse JSON content
  const char* command = doc["command"];

  if (strcmp(command, "set_profile") == 0){
    const char* profileName = doc["profile_name"];
    if (strcmp(profileName, "lettuce") == 0) currentProfile = &lettuceProfile;
    else if (strcmp(profileName, "tomato") == 0) currentProfile = &tomatoProfile;
    currentMode = AUTO; 
    Serial.printf("Profile set to: '%s'. Mode: AUTO\n", profileName);

  } else if (strcmp(command, "set_light_stage") == 0) {
    const char* stage = doc["stage"];
    if (strcmp(stage, "germination") == 0) currentLightRecipe = &currentProfile->germination_light;
    else if (strcmp(stage, "growth") == 0) currentLightRecipe = &currentProfile->growth_light;
    else if (strcmp(stage, "flowering") == 0) currentLightRecipe = &currentProfile->flowering_light;
    Serial.printf("Light stage set to: '%s'.\n", stage);
  }
  else if (strcmp(command, "manual_override") == 0){
    currentMode = MANUAL;  // whatever comand manual we receive activate manual mode

    // The command can afect only one actuator, so we read the JSON
    if (!doc["pump"].isNull()) currentManualState.pump = doc["pump"];
    if (!doc["fan_intake"].isNull()) currentManualState.fan_intake = doc["fan_intake"];
    if (!doc["fan_exhaust"].isNull()) currentManualState.fan_exhaust = doc["fan_exhaust"];
    if (!doc["window"].isNull()) currentManualState.window = doc["window"];
    if (!doc["led_rgb"].isNull()) {
      currentManualState.led_rgb.r = doc["led_rgb"]["r"];
      currentManualState.led_rgb.g = doc["led_rgb"]["g"];
      currentManualState.led_rgb.b = doc["led_rgb"]["b"];
    }
    
    xQueueSend(actuatorCommandQueue, &currentManualState, 0);
    Serial.printf("Manual command applied.");
  }

}

// --- Function to reconnect to MQTT broker ---
void reconect(){
  while(!client.connected()){
    Serial.print("Attempting MQTT conncetion...");
    if (client.connect(mqtt_client_id, mqtt_user, mqtt_password)){
      Serial.println("connected");
      client.subscribe(topic_commands); 
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
  }
}

// --- Task to read sensors ---
void TaskSensors(void *pvParameters) {
  SensorData data;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(5000); // Read sensors every 5 seconds

  const int dry = 930;
  const int wet = 550;

  for (;;){
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    data.temperature = dht.readTemperature();
    data.humidity = dht.readHumidity();

    // This semaphore is used to avoid conflicts on the I2C bus
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      data.lux = round(lightSensor.readLightLevel() *100.0) / 100.0;
      data.ppfd = round(luxToPPFD(data.lux) * 100.0) / 100.0;
      xSemaphoreGive(i2cMutex);
    }

    float soil_sum = 0;

    for (int i = 0; i < 4; i++){
      int raw_value = analogRead(soil_sensors_pin[i]);
      int mapped_value = map(raw_value, wet, dry, 100, 0);
      data.soil_moisture[i] = constrain(mapped_value, 0, 100);
      soil_sum += data.soil_moisture[i];
    }
    data.soil_avg = soil_sum / 4.0;

    // Send data to queues
    xQueueSend(sensorDataQueue, &data, portMAX_DELAY);
    xQueueSend(telemetryQueue, &data, portMAX_DELAY);
    
    Serial.println("[TaskSensors] Readings sent to queue.");

  }
}

// --- Task to apply control logic ---
void TaskControl(void *pvParameters){
  (void) pvParameters;
  SensorData receivedData;
  ActuatorState newState = {0};

  for(;;){
    if (xQueueReceive(sensorDataQueue, &receivedData, portMAX_DELAY) == pdPASS) {
      
      // If we are in manual mode, skip control logic
      if (currentMode == MANUAL) {
        continue;
      }

      // TODO: See if a control logic like PID or reinforcement learning with TinyML will be implemented.

      // Control logic based on currentProfile
      if (receivedData.temperature > currentProfile->temp_max){
        newState.fan_exhaust = HIGH;
        newState.fan_intake = HIGH;
      } else {
        newState.fan_exhaust = LOW;
        newState.fan_intake = LOW;
      }
      
      // Pump control
      newState.pump = (receivedData.soil_avg < currentProfile->soil_moisture_target) ? HIGH : LOW;

      // Servo control for window 
      newState.window = (receivedData.temperature > currentProfile->temp_max - 2) ? 1 : 0; 

      
      // LED Strip control
      // TODO: Add logic based on time of day using NTP
      if (receivedData.lux < currentProfile->light_lux_target) {
        newState.led_rgb = *currentLightRecipe;
      } else {
        newState.led_rgb = {0, 0, 0}; // Turn off if above target
      }

      xQueueSend(actuatorCommandQueue, &newState, portMAX_DELAY);
    }
  }
}

// --- Task to control actuators ---
void TaskActuators(void *pvParameters){
  const int tolerance = 4;
  ActuatorState state;

  for (;;){
    if(xQueueReceive(actuatorCommandQueue, &state, portMAX_DELAY) == pdPASS){
      
      // Debug
      Serial.println("[TaskActuators] New actuator state received.");
      
      digitalWrite(pin_pump, state.pump);
      digitalWrite(pin_fan_intake, state.fan_intake);
      digitalWrite(pin_fan_exhaust, state.fan_exhaust);

      int currentPos = servo.read();
      //Serial.printf("[Servo] Command: %d, Current: %d, Open: %d, Close: %d\n", state.window, currentPos, window_open, window_close);

      if(state.window == 1 && abs(currentPos - window_close) <= tolerance){
        for (int pos = window_close; pos >= window_open; pos -= 1) {
          servo.write(pos);
          vTaskDelay(pdMS_TO_TICKS(30));
        }
      }
      if(state.window == 0 && abs(currentPos - window_open) <= tolerance){
        for (int pos = window_open; pos <= window_close; pos += 1) {
          servo.write(pos);
          vTaskDelay(pdMS_TO_TICKS(30));
        }
      }

      analogWrite(pin_led_r, state.led_rgb.r);
      analogWrite(pin_led_g, state.led_rgb.g);
      analogWrite(pin_led_b, state.led_rgb.b);
      
    }
  }
}

// --- Task to handle communications ---
void TaskComm(void *pvParameters){

  SensorData telemetryData;
  // Conect to WiFi
  setup_wifi();

  // Configure MQTT client
  espClient.setInsecure(); 
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); 

  for(;;){
    // Ensure MQTT connection
    if (!client.connected()) {
      reconect();
    }
    client.loop();
   
    // Publish telemetry if data is available
    if (xQueueReceive(telemetryQueue, &telemetryData, 0) == pdPASS) {
      // Add data to JSON document 
      JsonDocument doc;
      doc["profile"] = currentProfile->name;
      doc["mode"] = (currentMode == AUTO) ? "AUTO" : "MANUAL";
      doc["temperature"] = telemetryData.temperature;
      doc["humidity"] = telemetryData.humidity;
      doc["lux"] = telemetryData.lux;
      doc["ppfd"] = telemetryData.ppfd;
      doc["soil_avg"] = telemetryData.soil_avg;

      JsonArray soil_moistures = doc["soil_moistures"].to<JsonArray>();
      for (int i = 0; i < 4; i++) {
        soil_moistures.add(telemetryData.soil_moisture[i]);
      }

      char buffer[256];
      serializeJson(doc, buffer);
      client.publish(topic_telemetry, buffer);

      Serial.printf("Published to %s: %s\n", topic_telemetry, buffer);
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to avoid busy loop
  }
}

// --- Setup function ---
void setup() {
  delay(1000);

  // Start Serial connection 
  Serial.begin(115200);
  Wire.begin();       

  // Pin modes
  pinMode(pin_pump, OUTPUT);
  pinMode(pin_fan_intake, OUTPUT);
  pinMode(pin_fan_exhaust, OUTPUT);
  pinMode(pin_led_r, OUTPUT);
  pinMode(pin_led_g, OUTPUT);
  pinMode(pin_led_b, OUTPUT);

  // Initialize sensors 
  Serial.println("Initializing sensors...");

  if (!lightSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE_2)){
    Serial.println("Could not find a valid BH1750 sensor, check wiring!");
    while (1);
  }
  
  if (!initDHT()) {
    Serial.println("Could not find a valid DHT22 sensor, check wiring!");
    while(1);
  } 
  servo.attach(pin_servo);
  servo.write(window_close);


  Serial.println("Sensor initialization complete.");

  // Create queues and mutex
  sensorDataQueue = xQueueCreate(5, sizeof(SensorData));
  actuatorCommandQueue = xQueueCreate(5, sizeof(ActuatorState));
  telemetryQueue = xQueueCreate(5, sizeof(SensorData));
  i2cMutex = xSemaphoreCreateMutex();
  
  // Create tasks
  xTaskCreatePinnedToCore(TaskSensors, "SensorTask", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(TaskControl, "ControlTask", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskActuators, "ActuatorTask", 2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskComm, "CommTask", 5120, NULL, 1, NULL, 1);

  Serial.println("FreeRTOS Setup complete.");
}

// --- Main loop function ---
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000)); 
