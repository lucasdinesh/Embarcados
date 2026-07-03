/*
  Projeto: Monitoramento de Servidor com ESP32, BMP280 e Controle IR Universal
*/

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRac.h>           
#include <HTTPClient.h>     
#include <ArduinoJson.h>    

// --- Configurações de Rede e Servidor Local ---
const char* ssid = "NOME_DA_SUA_REDE";
const char* password = "SENHA_DA_REDE";
const char* urlMeuServidor = "http://192.168.1.100:8000"; // IP do seu PC/Servidor rodando o Python

// --- Definições de Hardware ---
const uint16_t kIrLed = 4;        // Pino GPIO 4 conectado ao módulo Transmissor IR
#define BMP_SDA 21                // Pino I2C SDA
#define BMP_SCL 22                // Pino I2C SCL

// --- Limites e Configurações Dinâmicas (Baixados do Servidor) ---
int temperaturaAlerta = 26;   
int temperaturaDesligar = 22;
int temperaturaAlvoAC = 20;
decode_type_t protocoloAC = decode_type_t::LG; 

// --- Instâncias dos Objetos ---
Adafruit_BMP280 bmp;
AsyncWebServer server(80);
IRac universalAC(kIrLed); 

// --- Variáveis Globais ---
float temperaturaAtual = 0.0;
bool arCondicionadoLigado = false;
unsigned long ultimoTempoLeitura = 0;
unsigned long ultimoTempoTelemetria = 0; 
unsigned long ultimoTempoConfig = 0;     

void setup() {
  Serial.begin(115200);
  
  universalAC.next.protocol = protocoloAC; 
  Serial.println("Emissor IR Universal Inicializado.");

  if (!bmp.begin(0x76)) { // Tente 0x77 se falhar
    Serial.println("Erro: Sensor BMP280 não encontrado.");
  } else {
    Serial.println("Sensor BMP280 pronto.");
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Conectado!");

  // Busca as regras do servidor logo ao ligar
  buscarConfiguracaoServidor();

  // Rotas da API local do próprio ESP32
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{\"temperatura\": " + String(temperaturaAtual) + ", \"ar_ligado\": " + (arCondicionadoLigado ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  server.on("/ar/ligar", HTTP_POST, [](AsyncWebServerRequest *request){
    ligarArCondicionado("Acionamento manual via API do ESP32.");
    request->send(200, "text/plain", "Comando IR enviado.");
  });

  server.begin();
}

void loop() {
  // 1. Lê o sensor a cada 10 segundos
  if (millis() - ultimoTempoLeitura > 10000) {
    temperaturaAtual = bmp.readTemperature();
    Serial.print("Temp: "); Serial.println(temperaturaAtual);

    String logMsg = "";

    // Lógica de Segurança
    if (temperaturaAtual >= temperaturaAlerta && !arCondicionadoLigado) {
      logMsg = "ALERTA: Temperatura (" + String(temperaturaAtual) + "C) ultrapassou o limite. Ligando AC.";
      Serial.println(logMsg);
      ligarArCondicionado(logMsg);
      ultimoTempoTelemetria = millis(); 
    } 
    else if (temperaturaAtual <= temperaturaDesligar && arCondicionadoLigado) {
      logMsg = "INFO: Temperatura (" + String(temperaturaAtual) + "C) atingiu o alvo. Desligando AC.";
      Serial.println(logMsg);
      desligarArCondicionado(logMsg);
      ultimoTempoTelemetria = millis(); 
    }
    
    ultimoTempoLeitura = millis();
  }

  // 2. Envia telemetria de rotina para desenhar o gráfico (a cada 60 segundos)
  if (millis() - ultimoTempoTelemetria > 60000) {
    enviarTelemetriaBackend("ROTINA", "Temperatura normal.");
    ultimoTempoTelemetria = millis();
  }

  // 3. Atualiza as configurações do servidor (a cada 5 minutos)
  if (millis() - ultimoTempoConfig > 300000) {
    buscarConfiguracaoServidor();
    ultimoTempoConfig = millis();
  }
}

void ligarArCondicionado(String motivoLog) {
  universalAC.next.protocol = protocoloAC;         
  universalAC.next.degrees = temperaturaAlvoAC;                   
  universalAC.next.mode = stdAc::opmode_t::kCool;  
  universalAC.next.fanspeed = stdAc::fanspeed_t::kMax; 
  universalAC.sendAc();                            
  
  arCondicionadoLigado = true;
  enviarTelemetriaBackend("LIGAR_AC", motivoLog);
}

void desligarArCondicionado(String motivoLog) {
  universalAC.next.power = false; 
  universalAC.sendAc();
  
  arCondicionadoLigado = false;
  enviarTelemetriaBackend("DESLIGAR_AC", motivoLog);
}

void buscarConfiguracaoServidor() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    // A placa pede a configuração genérica (única do dashboard)
    String url = String(urlMeuServidor) + "/api/getConfig"; 
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == 200) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      
      if (doc.containsKey("marca")) {
        String marca = doc["marca"].as<String>();
        if (marca == "LG") protocoloAC = decode_type_t::LG;
        else if (marca == "SAMSUNG") protocoloAC = decode_type_t::SAMSUNG;
        else if (marca == "MIDEA") protocoloAC = decode_type_t::MIDEA;
        else if (marca == "DAIKIN") protocoloAC = decode_type_t::DAIKIN;
      }
      
      if (doc.containsKey("alerta")) temperaturaAlerta = doc["alerta"].as<int>();
      if (doc.containsKey("desligar")) temperaturaDesligar = doc["desligar"].as<int>();
      if (doc.containsKey("alvo")) temperaturaAlvoAC = doc["alvo"].as<int>();
      
      Serial.println("Configurações atualizadas via Servidor.");
    }
    http.end();
  }
}

void enviarTelemetriaBackend(String acao, String logMsg) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = String(urlMeuServidor) + "/api/telemetria";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["temperatura"] = temperaturaAtual;
    doc["ar_ligado"] = arCondicionadoLigado;
    doc["acao"] = acao;
    doc["log"] = logMsg;

    String jsonPayload;
    serializeJson(doc, jsonPayload);
    http.POST(jsonPayload);
    http.end();
  }
}