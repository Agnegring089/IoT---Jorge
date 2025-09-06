#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>   // HTTPS
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>

// ======================================================
// WiFi
// ======================================================
const char* SSID     = "AMF";
const char* PASSWORD = "amf@2025";

// ======================================================
// ENDPOINTS do seu Worker (ajuste o subdomínio se mudar)
// ======================================================
const char* URL_SCAN      = "https://sensorcomokvbanco.lazarolorenzi123.workers.dev/scan";
const char* URL_USER_BASE = "https://sensorcomokvbanco.lazarolorenzi123.workers.dev/user/";

// Modo padrão: usar /scan (recomendado, loga e diferencia códigos)
// Se quiser checagem “seca” sem log, mude para 0 e ele usará GET /user/:tagId?deviceId=...
#define USE_SCAN_ENDPOINT 1

// Identificação do dispositivo (DEVE bater com o deviceId fixado no servidor)
const char* DEVICE_ID = "esp32-porta";

// ======================================================
/* RC522 no ESP32 via VSPI
   Pinos padrão VSPI do ESP32: SCK=18, MISO=19, MOSI=23
   SS (SDA do RC522) = 5
   RST = 21
*/
#define SS_PIN   5
#define RST_PIN  21
MFRC522 rfid(SS_PIN, RST_PIN);

// Anti-repetição: evita spam enquanto o cartão fica encostado
String lastTag;
unsigned long lastTagAt = 0;
const unsigned long DEBOUNCE_MS = 1200;

// ======================================================
// Utils
// ======================================================
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
  }
  Serial.println("Falha ao conectar no WiFi.");
  return false;
}

// Faz POST JSON simples e retorna status code. Também volta o payload em respOut.
int httpPostJson(const char* url, const String& body, String& respOut) {
  if (WiFi.status() != WL_CONNECTED && !connectWiFi()) {
    return -1;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Para testes; em produção use setCACert com a CA do Cloudflare

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    Serial.println("http.begin() falhou (POST).");
    return -2;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");

  int code = http.POST(body);
  Serial.printf("HTTP %d\n", code);
  if (code > 0) {
    respOut = http.getString();
    if (respOut.length()) {
      Serial.println("Resposta:");
      Serial.println(respOut);
    }
  } else {
    Serial.printf("Erro HTTP: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code;
}

// Faz GET simples e retorna status code. Também volta o payload em respOut.
int httpGet(const String& url, String& respOut) {
  if (WiFi.status() != WL_CONNECTED && !connectWiFi()) {
    return -1;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Para testes

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    Serial.println("http.begin() falhou (GET).");
    return -2;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");

  int code = http.GET();
  Serial.printf("HTTP %d\n", code);
  if (code > 0) {
    respOut = http.getString();
    if (respOut.length()) {
      Serial.println("Resposta:");
      Serial.println(respOut);
    }
  } else {
    Serial.printf("Erro HTTP: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code;
}

// ======================================================
// Setup / Loop
// ======================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  connectWiFi();

  // VSPI explícito
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  // rfid.PCD_DumpVersionToSerial(); // opcional

  Serial.println("Pronto. Aproxime o cartão...");
}

void loop() {
  // Verifica presença e leitura
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(40);
    return;
  }

  String tagId = uidToHex(&rfid.uid);

  // Debounce
  if (tagId == lastTag && (millis() - lastTagAt) < DEBOUNCE_MS) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  lastTag = tagId;
  lastTagAt = millis();

  Serial.println("Tag lida: " + tagId);

  // =========================
  // 1) Usando /scan (recomendado)
  // =========================
#if USE_SCAN_ENDPOINT
  // Monta JSON: {"tagId":"...","deviceId":"...","type":"scan"}
  String body = String("{\"tagId\":\"") + tagId + "\",\"deviceId\":\"" + DEVICE_ID + "\",\"type\":\"scan\"}";
  String payload;
  int code = httpPostJson(URL_SCAN, body, payload);

  if (code == 200) {
    Serial.println("[OK] Acesso permitido: usuário ATIVO e device confere.");
  } else if (code == 401) {
    Serial.println("[BLOQUEADO] Device diferente do cadastrado para essa tag (wrong_device).");
  } else if (code == 403) {
    Serial.println("[!] Usuário encontrado porém INATIVO.");
  } else if (code == 404) {
    Serial.println("[X] Tag NÃO cadastrada.");
  } else if (code == 409) {
    Serial.println("[BLOQUEADO] Tag sem deviceId definido no servidor (device_not_bound).");
  } else if (code == 400) {
    Serial.println("[!] Requisição inválida (ex.: deviceId requerido para tag vinculada).");
  } else if (code < 0) {
    Serial.println("[!] Falha na requisição /scan.");
  } else {
    Serial.printf("[!] Resposta inesperada do servidor: %d\n", code);
  }

#else
  // =========================
  // 2) Usando GET /user/:tagId?deviceId=... (checagem sem log)
  // =========================
  String url = String(URL_USER_BASE) + tagId + "?deviceId=" + String(DEVICE_ID);
  String payload;
  int code = httpGet(url, payload);

  if (code == 200) {
    Serial.println("[OK] Usuário existe e device confere.");
  } else if (code == 401) {
    Serial.println("[BLOQUEADO] Device diferente do cadastrado para essa tag (wrong_device).");
  } else if (code == 404) {
    Serial.println("[X] Usuário/Tag NÃO encontrado no banco.");
  } else if (code < 0) {
    Serial.println("[!] Falha na requisição /user/:tagId.");
  } else {
    Serial.printf("[!] Resposta inesperada do servidor: %d\n", code);
  }
#endif

  // Finaliza comunicação com o cartão
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
