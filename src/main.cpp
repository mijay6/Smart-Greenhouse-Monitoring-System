#include <WiFiClientSecure.h>
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <BH1750.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <DHT.h>
#include <DHT_U.h>
#include <ArduinoJson.h>

// --- WIFI credentials ---
const char* ssid = "Internet Digi Daliei";
const char* password = "A;`AQpf/M7&k";

// --- MQTT Broker details ---
const char* mqtt_server = "abf09ccff5484da78e04fb5444f1faf9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;   // Secure port for MQTT over TLS
const char* mqtt_user = "Mijay";
const char* mqtt_password = "sHf728/ns@a8";
const char* mqtt_client_id = "ESP32_Greenhouse_Client";

// --- MQTT Topics ---
const char* topic_telemetry = "greenhouse/telemetry";
const char* topic_commands = "greenhouse/commands";

// TODO: los topicos de abajo quedaran obsoletos.
const char* topic_soil_moisture = "greenhouse/sensor/soil_moisture";
const char* topic_light_intensity = "greenhouse/sensor/env/light_intensity";
const char* topic_temperature = "greenhouse/sensor/env/temperature";
const char* topic_humidity = "greenhouse/sensor/env/humidity";
const char* topic_pump_control = "greenhouse/actuator/pump_control";
const char* topic_env_sensor = "greenhouse/sensor/env";

// --- Define GPIO pins ---
const uint8_t pin_pump = D4;  
const uint8_t pin_servo = D6;
const uint8_t pin_fan_intake = D3;
const uint8_t pin_fan_exhaust = D2;
const uint8_t pin_dht22 = 8;
const uint8_t soil_sensors_pin[] = {A0,A1,A2,A3}; // Array of soil moisture sensor pins
const uint8_t pin_led_r = D8;
const uint8_t pin_led_g = D7; 
const uint8_t pin_led_b = D9;

// --- Crop profiles (example for lettuce) ---
struct PlantProfile {
  float temp_max = 24.0;
  float temp_min = 18.0;
  float humidity_max = 70.0;
  int soil_moisture_target = 60;
  float light_lux_target = 15000.0;
};
PlantProfile currentProfile;

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
  int window_angle; // 0-180
  struct { int r, g, b; } led_rgb;
};

// --- Global objects ---
Servo servo;
BH1750 lightSensor; // Sensor connected over I2C
DHT dht(pin_dht22, DHT11);

WiFiClientSecure espClient;   
PubSubClient client(espClient);

// --- Task handlers ---
QueueHandle_t sensorDataQueue;
QueueHandle_t actuatorCommandQueue;
QueueHandle_t telemetryQueue;
SemaphoreHandle_t i2cMutex;

// --- Declare task functions ---
/// @param pvParameters 
void TaskSensors(void *pvParameters);
void TaskControl(void *pvParameters);
void TaskActuators(void *pvParameters); 
void TaskComm(void *pvParameters);

// lux to ppfd conversion
float luxToPPFD(float lux) {
  // Approximate conversion factor
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
const int msg_interval = 10000; // Interval between sending messages

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

  // --- Initialize NTP for time synchronization ---
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
    Serial.print((char)message[i]);   // send the caracters we receive in the serial monitor
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Y aqui en funcion de que mensaje recibimos en el topic, haremos una cosa u otra
  // POr ejemplo para el topic de light_intensity, (pero esto no es practico, solo ejemplo, habra que cambiarlo)
    if (String(topic) == topic_light_intensity){
    Serial.print("Changing light intensity to ");
    if(messageTemp == "on"){
      Serial.println("ON");
      // Activar el sensor de luz
    } else if(messageTemp == "off"){
      Serial.println("OFF");
      // Desactivar el sensor de luz
    }
  }
}

// --- Function to reconnect to MQTT broker ---
void reconect(){
  while(!client.connected()){
    Serial.print("Attempting MQTT conncetion...");
    if (client.connect(mqtt_client_id, mqtt_user, mqtt_password)){
      Serial.println("connected");
      // Subscribe
      // TODO: Habra que cambiar los topicos a los que se suscribe
      client.subscribe(topic_light_intensity); 
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
  (void) pvParameters;
  SensorData data;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(5000); // Read sensors every 5 seconds

  const int dry = 2650;
  const int wet = 1080;

  for (;;){
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    data.temperature = dht.readTemperature();
    data.humidity = dht.readHumidity();

    // This semaphore is used to avoid conflicts on the I2C bus
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      data.lux = lightSensor.readLightLevel();
      data.ppfd = luxToPPFD(data.lux);
      xSemaphoreGive(i2cMutex);
    }

    float soil_sum = 0;

    for (int i = 0; i < 4; i++){
      data.soil_moisture[i] = map(analogRead(soil_sensors_pin[i]), wet, dry, 100, 0);
      soil_sum += data.soil_moisture[i];
    }
    data.soil_avg = soil_sum / 4.0;

    // Send data to queue
    xQueueSend(sensorDataQueue, &data, portMAX_DELAY);
    xQueueSend(telemetryQueue, &data, portMAX_DELAY);
    
    //Debug
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
      // Control logic based on the plant profile 
      
      // TODO: Se implementara una logica de control como PID o por reinforcement learning con TinyML

      // Temperature control
      if (receivedData.temperature > currentProfile.temp_max){
        newState.fan_exhaust = HIGH;
        newState.fan_intake = HIGH;
      } else {
        newState.fan_exhaust = LOW;
        newState.fan_intake = LOW;
      }
      
      // Pump control
      newState.pump = (receivedData.soil_avg < currentProfile.soil_moisture_target) ? HIGH : LOW;

      // Servo control for window
      newState.window_angle = (receivedData.temperature > currentProfile.temp_max - 2) ? 90 : 0; // Open window if temp is 2 degrees above max

      // LED Strip control
      // TODO: Tambien ver en que datos basarnos lux o ppfd
      if (receivedData.lux < currentProfile.light_lux_target) {
        // TODO: Habra que ajustar el color y la intensidad en funcion de planta mediante su profil y tambien mediante lo que indique el usuario en la app si esta en germinacion, crecimiento o floracion
        newState.led_rgb = {255, 255, 255}; // White light if below target
      } else {
        newState.led_rgb = {0, 0, 0}; // Turn off if above target
      }

      xQueueSend(actuatorCommandQueue, &newState, portMAX_DELAY);
    }
  }
}

// --- Task to control actuators ---
void TaskActuators(void *pvParameters){
  (void) pvParameters;
  ActuatorState state;
  for (;;){
    if(xQueueReceive(actuatorCommandQueue, &state, portMAX_DELAY) == pdPASS){
      
      // Debug
      Serial.println("[TaskActuators] New actuator state received.");
      
      digitalWrite(pin_pump, state.pump);
      digitalWrite(pin_fan_intake, state.fan_intake);
      digitalWrite(pin_fan_exhaust, state.fan_exhaust);
      servo.write(state.window_angle);

      // TODO: ver si el analogWrite funciona bien
      analogWrite(pin_led_r, state.led_rgb.r);
      analogWrite(pin_led_g, state.led_rgb.g);
      analogWrite(pin_led_b, state.led_rgb.b);
      
    }
  }
}

// --- Task to handle communications ---
void TaskComm(void *pvParameters){
  (void) pvParameters;
  SensorData telemetryData;

  // Conect to WiFi
  setup_wifi();

  // conect to MQTT broker
  espClient.setInsecure(); 
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  for(;;){
    if (!client.connected()) {
      reconect();
    }
    client.loop();

   
    // Publis telemetry if data is available
    if (xQueueReceive(telemetryQueue, &telemetryData, 0) == pdPASS) {
      // Add data to JSON document 
      JsonDocument doc;
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
  Wire.begin();       // Initialize I2C
  while(!Serial);

  // Initialize GPIO pins 

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

  //if (!servo.attached()) {
  //}

  Serial.println("Sensor initialization complete.");

  // --- Connect to WiFi ---
  //setup_wifi();

  // --- Setup MQTT server details ---
  //espClient.setInsecure(); 
  //client.setServer(mqtt_server, mqtt_port);
  //client.setCallback(callback);

  // Create queues and mutex
  sensorDataQueue = xQueueCreate(5, sizeof(SensorData));
  actuatorCommandQueue = xQueueCreate(5, sizeof(ActuatorState));
  telemetryQueue = xQueueCreate(5, sizeof(SensorData));
  i2cMutex = xSemaphoreCreateMutex();
  
  // Create tasks
  xTaskCreatePinnedToCore(TaskSensors, "SensorTask", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(TaskControl, "ControlTask", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskActuators, "ActuatorTask", 2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskComm, "CommTask", 4096, NULL, 1, NULL, 1);

  
  Serial.println("FreeRTOS Setup complete.");

}
// --- Main loop function ---
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000)); // Keep the loop alive with a delay
  /**
  if (!client.connected()) {
    reconect();
  }

  client.loop(); // To keep the connection alive and to check for incoming messages

  unsigned long now = millis();
  if (now - lastMsg > msg_interval){ // Delay between sensor readings
    lastMsg = now;

    // --- Read sensors ---
    float temperature = dht.readTemperature(); // Celsius by default
    float humidity = dht.readHumidity();
    float lightIntensity = luxToPPFD(lightSensor.readLightLevel()); // convert from lux to PPFD


    // TODO: aqui que hacemos si nos da fallos de lectura?
    // if (isnan(temperature) || isnan(humidity)) {
    //   Serial.println("Failed to read from DHT sensor!");
    //  return;
    //}


    // --- Add variables to JSON document ---
    JsonDocument doc;

    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["light_intensity"] = lightIntensity;

    // -- Serialize JSON document and send ---
    char buffer[256];
    serializeJson(doc, buffer);
    client.publish(topic_env_sensor, buffer);
    Serial.printf("Published to %s: %s\n", topic_env_sensor, buffer);
  }

  **/

}
