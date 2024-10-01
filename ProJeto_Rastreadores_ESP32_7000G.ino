#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER 1024
#define SerialAT Serial1
#define SerialMon Serial

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <esp_adc_cal.h>
#include <Arduino.h>
#include <Preferences.h> // Biblioteca para armazenamento em Flash

const char apn[]  = "claro.com.br"; 
const char gprsUser[] = "claro";
const char gprsPass[] = "claro";

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
HttpClient http(client, "api.intellimetrics.tec.br", 80);

// Estrutura para armazenar data e hora
struct DateTime {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
};

// Variáveis globais para data e hora em string
char dtData[11]; // YYYY-MM-DD + '\0'
char dtHora[9];  // HH:MM:SS + '\0'

// Vamos medir a bateria
int vref = 1100;

// Variáveis de GPS
float lat, lon;
int nrSeq = 0;  // Inicializamos nrSeq a zero
int deepSleepCount = 0; // Contador de ciclos de sono profundo


Preferences preferences; // Objeto para gerenciar as preferências

#define UART_BAUD   115200
#define PIN_DTR     25
#define PIN_TX      27
#define PIN_RX      26
#define PWR_PIN     4
#define ADC_PIN     35
#define LED_PIN     12

// Função para obter a data e hora do modem
bool obterDataHora(DateTime &dateTime) {
  modem.sendAT("+CCLK?");
  if (modem.waitResponse(10000L, "+CCLK: ") == 1) {
    String response = modem.stream.readStringUntil('\n');
    response.trim();

    if (response.startsWith("\"")) {
      response = response.substring(1, response.lastIndexOf("\""));

      dateTime.year = 2000 + response.substring(0, 2).toInt();
      dateTime.month = response.substring(3, 5).toInt();
      dateTime.day = response.substring(6, 8).toInt();
      dateTime.hour = response.substring(9, 11).toInt();
      dateTime.minute = response.substring(12, 14).toInt();
      dateTime.second = response.substring(15, 17).toInt();

      snprintf(dtData, sizeof(dtData), "%04d-%02d-%02d", dateTime.year, dateTime.month, dateTime.day);
      snprintf(dtHora, sizeof(dtHora), "%02d:%02d:%02d", dateTime.hour, dateTime.minute, dateTime.second);

      return true;
    } else {
      Serial.println("Formato inesperado de resposta ao comando +CCLK.");
      return false;
    }
  } else {
    Serial.println("Falha ao obter data e hora através da operadora.");
    return false;
  }
}

void setup() {
  SerialMon.begin(115200);
  delay(10);

  String wakeupReason = getWakeupReason();

  Serial.println("Motivo do despertar: " + wakeupReason);

  // Iniciar preferences
  preferences.begin("storage", false);

  // Recuperar a sequência incrementada da memória
  nrSeq = preferences.getInt("nrSeq", 0); // Se não existir, começar de zero
  
  // Medir bateria
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    Serial.printf("eFuse Vref:%u mV", adc_chars.vref);
    vref = adc_chars.vref;
  } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
    Serial.printf("Two Point --> coeff_a:%umV coeff_b:%umV\n", adc_chars.coeff_a, adc_chars.coeff_b);
  } else {
    Serial.println("Default Vref: 1100mV");
  }

  SerialAT.begin(UART_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  // Setup Modem Power
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);
  delay(1000);
  digitalWrite(PWR_PIN, LOW);

  // Initialize modem
  SerialMon.println("Initializing modem...");
  if (!modem.restart()) {
    SerialMon.println("Failed to restart modem, trying without restart...");
  }

  SerialMon.println("Waiting for network...");
  if (!modem.waitForNetwork()) {
    SerialMon.println("Failed to connect to network");
    return;
  }

  // Connect to GPRS
  SerialMon.println("Connecting to GPRS...");
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println("GPRS connection failed");
    return;
  }
  SerialMon.println("GPRS connected!");

  DateTime now;
  if (obterDataHora(now)) {
    Serial.printf("Data: %s, Hora: %s\n", dtData, dtHora);
  }

  // Turn on GPS
  modem.sendAT("+CGPIO=0,48,1,1");
  if (modem.waitResponse() != 1) {
    SerialMon.println("Failed to turn on GPS power");
  }
  
  modem.enableGPS();

  for (int i = 0; i < 2; i++) {
    obterCoordenadasGPSlatlon();
    if (lat != 0.0 && lon != 0.0) {
      enviarParaAPI(lat, lon, nrSeq, wakeupReason);
      nrSeq++;
    } else {
      SerialMon.println("Failed to get valid GPS coordinates");
    }
    delay(2000);
  }

  modem.disableGPS();
  modem.sendAT("+CGPIO=0,48,1,0");
  if (modem.waitResponse() != 1) {
    SerialMon.println("Failed to turn off GPS power");
  }

  modem.gprsDisconnect();
  modem.poweroff();

  // Armazenar a sequência no final
  preferences.putInt("nrSeq", nrSeq);
}

void loop() {
 // enterDeepSleep();
 if (deepSleepCount < 5) {
    deepSleepCount++;
    enterDeepSleep();
  } else {
    nrSeq = 0; // Zerar nrSeq antes de enviar os dados
    // Aqui você poderia fazer outra ação, se necessário, antes de entrar em sono profundo novamente.
    preferences.putInt("nrSeq", nrSeq);
  
    // Envio de dados ou outras ações que você deseja executar após 5 ciclos de sono profundo.
    // Por exemplo você pode reconfigurar o dispositivo ou fazer um reset etc.
    enterDeepSleep();
  }
}

String getWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0 RTC_IO BTN";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1 RTC_CNTL";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP PROGRAM";
    default: return "NO DS CAUSE";
  }
}

void obterCoordenadasGPSlatlon() {
  float obtainedLat, obtainedLon;
  for (int attempts = 0; attempts < 10; attempts++) {
    if (modem.getGPS(&obtainedLat, &obtainedLon)) {
      lat = obtainedLat;
      lon = obtainedLon;
      SerialMon.printf("Lat: %f, Lon: %f\n", lat, lon);
      return;
    } else {
      delay(2000);
    }
  }
  lat = 0.0;
  lon = 0.0;
}

void enviarParaAPI(float latitude, float longitude, int sequence, const String& reason) {
  StaticJsonDocument<300> jsonDoc;
  jsonDoc["cdDispositivo"] = 1;
  jsonDoc["dsArquivo"] = reason;  // Enviar o motivo do wakeup
  jsonDoc["dsLat"] = String(latitude, 6);
  jsonDoc["dsLong"] = String(longitude, 6);
  jsonDoc["dsModelo"] = "ESP32 SIM7000G";
  jsonDoc["dtData"] = dtData;
  jsonDoc["dtHora"] = dtHora;
  jsonDoc["nrBat"] = medirNivelBateria();
  jsonDoc["nrSeq"] = sequence;
  jsonDoc["nrTemp"] = 18;

  JsonArray sensores = jsonDoc.createNestedArray("sensores");

  JsonObject sensor1 = sensores.createNestedObject();
  sensor1["cdSensor"] = 1;
  sensor1["nrValor"] = 11;

  JsonObject sensor2 = sensores.createNestedObject();
  sensor2["cdSensor"] = 2;
  sensor2["nrValor"] = 0;

  String requestBody;
  serializeJson(jsonDoc, requestBody);

  http.beginRequest();
  http.post("/Posicao");
  http.sendHeader("Content-Type", "application/json");
  http.sendHeader("Content-Length", requestBody.length());
  http.beginBody();
  http.print(requestBody);
  http.endRequest();

  int statusCode = http.responseStatusCode();
  String response = http.responseBody();
  SerialMon.print("Status code: ");
  SerialMon.println(statusCode);
  SerialMon.print("Response: ");
  SerialMon.println(response);
}

float medirNivelBateria() {
  uint16_t v = analogRead(ADC_PIN);
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
  String voltage = "Voltage :" + String(battery_voltage) + "V\n";
  Serial.println(voltage);

  return battery_voltage;
}

void enterDeepSleep() {
  // Desligar LED para economizar energia
 
  digitalWrite(PIN_DTR, LOW);
  digitalWrite(PIN_TX, LOW);
  digitalWrite(PIN_RX, LOW);
  digitalWrite(PWR_PIN, LOW);
  digitalWrite(LED_PIN, LOW); // Desligar alimentação do modem para economizar energia
  delay(5000);
  
  // Configurar para 6 horas de sono profundo (6 horas * 60 minutos * 60 segundos * 1.000.000 microssegundos)
//  esp_sleep_enable_timer_wakeup(6ULL * 60ULL * 60ULL * 1000000ULL); // 6 HORAS
  esp_sleep_enable_timer_wakeup(60ULL * 60ULL * 1000000ULL); // 60 minutos
  //esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);  // 60 segundos
  esp_deep_sleep_start();
}
