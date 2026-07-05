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
const char* ssid = "LUFE";
const char* password = "190917LF";
const char* urlMeuServidor = "http://192.168.15.5:80"; // IP do seu PC/Servidor rodando o Python

// --- Definições de Hardware ---
const uint16_t kIrLed = 4;        // Pino GPIO 4 conectado ao módulo Transmissor IR
#define BMP_SDA 21                // Pino I2C SDA
#define BMP_SCL 22                // Pino I2C SCL

// --- Limites e Configurações Dinâmicas (Baixados do Servidor) ---
int temperaturaAlerta = 26;   
int temperaturaDesligar = 22;
int temperaturaAlvoAC = 20;
bool modoTeste = false;
decode_type_t protocoloAC = decode_type_t::LG; 

// --- Instâncias dos Objetos ---
Adafruit_BMP280 bmp;
AsyncWebServer server(80);
IRac universalAC(kIrLed); 
IRsend irsend(kIrLed); // Objeto irsend criado para a TV

// --- Variáveis Globais ---
float temperaturaAtual = 0.0;
bool arCondicionadoLigado = false;
bool estadoTesteTV = false; 
unsigned long ultimoTempoLeitura = 0;
unsigned long ultimoTempoTelemetria = 0; 
unsigned long ultimoTempoConfig = 0;  
unsigned long ultimaVezTvEnviou = 0;
const unsigned long intervaloTv = 30000; // 30 segundos para o modo teste

void setup() {
  Serial.begin(115200);
  
  universalAC.next.protocol = protocoloAC; 
  irsend.begin(); // Inicia o pino IR para enviar códigos brutos
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

float lerTemperaturaAssertiva() {
  float leituras[5];
  int qtdValidas = 0;

  // 1. Coleta 5 amostras aplicando o Filtro de Sanidade
  for (int i = 0; i < 5; i++) {
    float t = bmp.readTemperature();
    
    // Só aceita se for um número válido E estiver dentro de uma faixa aceitável para o ambiente
    if (!isnan(t) && t >= 0.0 && t <= 50.0) {
      leituras[qtdValidas] = t;
      qtdValidas++;
    }
    delay(150); // Curto intervalo entre as amostras
  }

  // Se o sensor falhar completamente e não der nenhuma leitura na faixa aceitável
  if (qtdValidas == 0) {
    Serial.println("Erro crítico: Todas as leituras do sensor foram inválidas ou absurdas!");
    return temperaturaAtual; // Mantém a última temperatura conhecida por segurança
  }

  // 2. Filtro de Mediana: Ordena o array do menor para o maior
  for (int i = 0; i < qtdValidas - 1; i++) {
    for (int j = i + 1; j < qtdValidas; j++) {
      if (leituras[i] > leituras[j]) {
        float temp = leituras[i];
        leituras[i] = leituras[j];
        leituras[j] = temp;
      }
    }
  }

  // 3. Retorna o valor do meio (Mediana)
  int indiceMediana = qtdValidas / 2;
  return leituras[indiceMediana];
}

void loop() {
  // 1. Lê o sensor a cada 10 segundos com estabilização
  if (millis() - ultimoTempoLeitura > 10000) {
    temperaturaAtual = lerTemperaturaAssertiva();
    Serial.print("Temp Filtrada: "); Serial.println(temperaturaAtual);

    String logMsg = "";

    // 2. Lógica do Controle IR da TV (Executa a cada 30 segundos quando em modo teste)
    if (modoTeste) {
      if (millis() - ultimaVezTvEnviou > intervaloTv) {
        if (estadoTesteTV) {
            logMsg = "TESTE: Enviando comando para DESLIGAR a TV LG.";
            Serial.println(logMsg);
            irsend.sendNEC(0x20DF10EF, 32); 
            enviarTelemetriaBackend("TESTE_TV_OFF", logMsg);
            estadoTesteTV = false;
        } else {
            logMsg = "TESTE: Enviando comando para LIGAR a TV LG.";
            Serial.println(logMsg);
            irsend.sendNEC(0x20DF10EF, 32); 
            enviarTelemetriaBackend("TESTE_TV_ON", logMsg);
            estadoTesteTV = true;
        }
        ultimaVezTvEnviou = millis();
      }
    } else {
      // Lógica normal de AC
      if (temperaturaAtual >= temperaturaAlerta && !arCondicionadoLigado) {
        logMsg = "ALERTA: Temperatura (" + String(temperaturaAtual) + "C) ultrapassou o limite. Ligando AC.";
        ligarArCondicionado(logMsg);
      } 
      else if (temperaturaAtual <= temperaturaDesligar && arCondicionadoLigado) {
        logMsg = "INFO: Temperatura (" + String(temperaturaAtual) + "C) atingiu o alvo. Desligando AC.";
        desligarArCondicionado(logMsg);
      }
    }
    ultimoTempoLeitura = millis();
  }

  // 3. Envia telemetria de rotina (a cada 60 segundos)
  if (millis() - ultimoTempoTelemetria > 60000) {
    enviarTelemetriaBackend("ROTINA", "Temperatura normal.");
    ultimoTempoTelemetria = millis();
  }

  // 4. Atualiza as configurações do servidor (a cada 5 minutos)
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
      
      if (doc.containsKey("modo_teste")) {
          modoTeste = doc["modo_teste"].as<bool>();
      }
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