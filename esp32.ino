#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>

#define DHT_PIN 4
#define DHT_TYPE DHT22
#define RGB_LED_PIN 48

const char* WIFI_NAME = "___username____";
const char* WIFI_PASSWORD = "____Password___";


const char* SERVER_URL =
  "http://192.168.1.100:3000/api/readings";

const char* DEVICE_ID = "esp32s3-fenugreek-01";
const char* CROP_NAME = "fenugreek_microgreens";

DHT dht(DHT_PIN, DHT_TYPE);

enum DataMode
{
  SIMULATED_ONLY,
  DHT22_ONLY,
  DHT22_WITH_SIMULATED_FALLBACK
};

/*
 * Select the operating mode here.
 *
 * SIMULATED_ONLY:
 * Always generates test data.
 *
 * DHT22_ONLY:
 * Reads only the actual sensor.
 * Nothing is sent if the sensor fails.
 *
 * DHT22_WITH_SIMULATED_FALLBACK:
 * Tries the DHT22 first.
 * Uses simulated data if the DHT22 fails.
 */
const DataMode DATA_MODE = SIMULATED_ONLY;

struct SensorReading
{
  float temperature;
  float humidity;

  bool valid;
  bool simulated;

  String source;
  String error;
};

unsigned long readingNumber = 0;
unsigned long previousReadingTime = 0;

const unsigned long READING_INTERVAL = 10000;

// Values retained between simulated readings.
float simulatedTemperature = 27.0;
float simulatedHumidity = 68.0;

void setLed(uint8_t red, uint8_t green, uint8_t blue)
{
  rgbLedWrite(RGB_LED_PIN, red, green, blue);
}

/*
 * Generates slowly changing simulated data.
 *
 * The random-walk method is more realistic than generating
 * completely unrelated values every time.
 */
SensorReading generateSimulatedReading()
{
  SensorReading reading;

  float temperatureChange =
    random(-25, 26) / 100.0;

  float humidityChange =
    random(-60, 61) / 100.0;

  simulatedTemperature += temperatureChange;
  simulatedHumidity += humidityChange;

  simulatedTemperature =
    constrain(simulatedTemperature, 20.0, 40.0);

  simulatedHumidity =
    constrain(simulatedHumidity, 30.0, 95.0);

  reading.temperature = simulatedTemperature;
  reading.humidity = simulatedHumidity;

  reading.valid = true;
  reading.simulated = true;

  reading.source = "simulated_test";
  reading.error = "";

  return reading;
}

/*
 * Reads actual temperature and humidity from the
 * DHT22 connected to ESP32-S3 GPIO4.
 */
SensorReading readDHT22Reading()
{
  SensorReading reading;

  reading.temperature = NAN;
  reading.humidity = NAN;

  reading.valid = false;
  reading.simulated = false;

  reading.source = "dht22_gpio4";
  reading.error = "";

  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(temperature) || isnan(humidity))
  {
    reading.error = "DHT22 communication failed";
    return reading;
  }

  // Basic range validation for DHT22.
  if (temperature < -40.0 || temperature > 80.0)
  {
    reading.error = "Temperature outside DHT22 range";
    return reading;
  }

  if (humidity < 0.0 || humidity > 100.0)
  {
    reading.error = "Humidity outside valid range";
    return reading;
  }

  reading.temperature = temperature;
  reading.humidity = humidity;
  reading.valid = true;

  return reading;
}

/*
 * Selects the reading source based on DATA_MODE.
 */
SensorReading collectReading()
{
  if (DATA_MODE == SIMULATED_ONLY)
  {
    Serial.println("Mode: simulated data");

    return generateSimulatedReading();
  }

  Serial.println("Attempting actual DHT22 reading...");

  SensorReading actualReading = readDHT22Reading();

  if (actualReading.valid)
  {
    Serial.println("Actual DHT22 reading successful.");

    return actualReading;
  }

  Serial.print("DHT22 error: ");
  Serial.println(actualReading.error);

  if (DATA_MODE == DHT22_ONLY)
  {
    return actualReading;
  }

  Serial.println("Using simulated fallback data.");

  SensorReading fallbackReading =
    generateSimulatedReading();

  fallbackReading.source = "simulated_fallback";
  fallbackReading.error = actualReading.error;

  return fallbackReading;
}

void connectToWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  setLed(0, 0, 20);

  Serial.print("Connecting to Wi-Fi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  unsigned long connectionStarted = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - connectionStarted < 20000
  )
  {
    Serial.print(".");
    delay(500);
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Wi-Fi connected.");

    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());

    setLed(0, 20, 0);
  }
  else
  {
    Serial.println("Wi-Fi connection failed.");

    setLed(20, 0, 0);
  }
}

String createJson(const SensorReading& reading)
{
  readingNumber++;

  String json = "{";

  json += "\"device_id\":\"";
  json += DEVICE_ID;
  json += "\",";

  json += "\"crop\":\"";
  json += CROP_NAME;
  json += "\",";

  json += "\"reading_number\":";
  json += readingNumber;
  json += ",";

  json += "\"uptime_ms\":";
  json += millis();
  json += ",";

  json += "\"air_temperature_c\":";
  json += String(reading.temperature, 2);
  json += ",";

  json += "\"air_humidity_percent\":";
  json += String(reading.humidity, 2);
  json += ",";

  json += "\"simulated\":";
  json += reading.simulated ? "true" : "false";
  json += ",";

  json += "\"source\":\"";
  json += reading.source;
  json += "\",";

  json += "\"sensor_error\":\"";
  json += reading.error;
  json += "\"";

  json += "}";

  return json;
}

bool postReading(const SensorReading& reading)
{
  if (!reading.valid)
  {
    Serial.println("Invalid reading: POST skipped.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    connectToWiFi();
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("No Wi-Fi: POST skipped.");
    return false;
  }

  String json = createJson(reading);

  Serial.println();
  Serial.println("Sending JSON:");
  Serial.println(json);

  setLed(20, 0, 20);

  WiFiClient client;
  HTTPClient http;

  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  if (!http.begin(client, SERVER_URL))
  {
    Serial.println("Unable to initialize HTTP request.");

    setLed(20, 0, 0);
    return false;
  }

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  int responseCode = http.POST(json);

  if (responseCode > 0)
  {
    Serial.print("HTTP response code: ");
    Serial.println(responseCode);

    String responseBody = http.getString();

    Serial.print("Server response: ");
    Serial.println(responseBody);

    http.end();

    if (responseCode >= 200 && responseCode < 300)
    {
      setLed(0, 20, 0);
      return true;
    }

    setLed(20, 10, 0);
    return false;
  }

  Serial.print("HTTP request failed: ");
  Serial.println(http.errorToString(responseCode));

  http.end();

  setLed(20, 0, 0);
  return false;
}

void printReading(const SensorReading& reading)
{
  Serial.println();
  Serial.println("Sensor reading");
  Serial.println("--------------------------");

  Serial.print("Source: ");
  Serial.println(reading.source);

  Serial.print("Simulated: ");
  Serial.println(reading.simulated ? "Yes" : "No");

  Serial.print("Valid: ");
  Serial.println(reading.valid ? "Yes" : "No");

  if (reading.valid)
  {
    Serial.print("Temperature: ");
    Serial.print(reading.temperature, 2);
    Serial.println(" °C");

    Serial.print("Humidity: ");
    Serial.print(reading.humidity, 2);
    Serial.println(" %");
  }

  if (reading.error.length() > 0)
  {
    Serial.print("Sensor error: ");
    Serial.println(reading.error);
  }

  Serial.println("--------------------------");
}

void collectAndSend()
{
  setLed(20, 20, 0);

  SensorReading reading = collectReading();

  printReading(reading);

  postReading(reading);
}

void setup()
{
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("ESP32-S3 environmental data collector");

  // Seed the random generator using the ESP32 hardware RNG.
  randomSeed(esp_random());

  dht.begin();

  connectToWiFi();

  // Allow the DHT22 to stabilize.
  delay(3000);

  collectAndSend();

  previousReadingTime = millis();
}

void loop()
{
  if (
    millis() - previousReadingTime >=
    READING_INTERVAL
  )
  {
    previousReadingTime = millis();

    collectAndSend();
  }
}