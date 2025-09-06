#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>   // para HTTPS
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>

// ===== WIFI =====
const char* SSID     = "AMF";
const char* PASSWORD = "amf@2025";

// ===== WORKER URLS (HTTPS) =====
const char* URL_REGISTER  = "https://sensorcomokvbanco.lazarolorenzi123.workers.dev/register";
const char* URL_USER_BASE = "https://sensorcomokvbanco.lazarolorenzi123.workers.dev/user/";

// ===== IDENTIDADE DO DEVICE (fixa para a tag) =====
const char* DEVICE_ID = "esp32-porta-sala";   // <- defina o ID fixo do device aqui

// (Opcional) Chave admin para override do deviceId no servidor
const char* ADMIN_KEY = ""; // se tiver ADMIN_KEY no Worker, coloque aqui; senão deixe vazio ""

// ===== MFRC522 PINS (ESP32 VSPI) =====
#define SS_PIN   5    // SDA/SS do RC522
#define RST_PIN  21   // pode ser 27, 22, etc. (GPIO livre)

MFRC522 rfid(SS_PIN, RST_PIN);

// Converte UID para HEX maiúsculo
String uidToHex(const MFRC522::Uid* uid) {
  String s;
  s.reserve(uid->size * 2);
  for (byte i = 0; i < uid->size; i++) {
    if (uid->uidByte[i] < 0x10) s += '0';
    s += String(uid->uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

bool connectWiFi(uint32_t timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Conectando ao WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK -> ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("Falha ao conectar no WiFi.");
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  connectWiFi();

  // ESP32 VSPI padrão: SCK=18, MISO=19, MOSI=23, SS=5
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  // rfid.PCD_DumpVersionToSerial(); // opcional

  Serial.println("ESP Cadastro pronto, aproxime o cartão...");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  String tagId = uidToHex(&rfid.uid);
  Serial.println("Lido tag: " + tagId);

  // JSON de cadastro já incluindo o deviceId FIXO
  String json = String("{\"tagId\":\"") + tagId +
                "\",\"name\":\"Maria Silva\",\"userId\":\"maria01\"," +
                "\"deviceId\":\"" + String(DEVICE_ID) + "\"}";

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // ⚠️ testes; em produção use client.setCACert(...)

    // ===== 1) Tenta registrar com deviceId fixo =====
    HTTPClient http;
    http.setTimeout(10000); // 10s
    if (!http.begin(client, URL_REGISTER)) {
      Serial.println("http.begin() falhou (POST /register).");
    } else {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Accept", "application/json");
      http.addHeader("Connection", "close");
      if (ADMIN_KEY[0] != '\0') http.addHeader("x-api-key", ADMIN_KEY); // permite override no /register

      int httpCode = http.POST(json);
      Serial.print("HTTP (POST /register) ");
      Serial.println(httpCode);

      String payload = "";
      if (httpCode > 0) {
        payload = http.getString();
        Serial.println("Resposta:");
        Serial.println(payload);
      } else {
        Serial.print("Erro na requisição: ");
        Serial.println(http.errorToString(httpCode));
      }
      http.end();

      // ===== 2) Se houve conflito de device (409), tenta bind administrativo (PATCH) =====
      if (httpCode == 409 && ADMIN_KEY[0] != '\0') {
        Serial.println("Conflito de deviceId. Tentando PATCH /user/:tagId/bind-device (modo admin)...");
        String urlBind = String(URL_USER_BASE) + tagId + "/bind-device";
        String bodyBind = String("{\"deviceId\":\"") + String(DEVICE_ID) + "\"}";

        HTTPClient http2;
        http2.setTimeout(10000);
        if (!http2.begin(client, urlBind)) {
          Serial.println("http.begin() falhou (PATCH bind-device).");
        } else {
          http2.addHeader("Content-Type", "application/json");
          http2.addHeader("Accept", "application/json");
          http2.addHeader("Connection", "close");
          http2.addHeader("x-api-key", ADMIN_KEY);

          int code2 = http2.sendRequest("PATCH", bodyBind);
          Serial.print("HTTP (PATCH bind-device) ");
          Serial.println(code2);
          if (code2 > 0) {
            String resp2 = http2.getString();
            Serial.println("Resposta (PATCH):");
            Serial.println(resp2);
          } else {
            Serial.print("Erro PATCH: ");
            Serial.println(http2.errorToString(code2));
          }
          http2.end();
        }
      }
    }

  } else {
    Serial.println("Sem WiFi. Tentando reconectar...");
    connectWiFi();
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(2000); // evita spam
}
