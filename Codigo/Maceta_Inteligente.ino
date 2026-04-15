#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "AiEsp32RotaryEncoder.h"
#include "time.h" 

// --- CONFIGURACIÓN FIREBASE Y HORA ---
const String FIREBASE_URL = "https://panel-vfd-monitor-default-rtdb.firebaseio.com/macetero_1.json";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -14400; // GMT-4
const int   daylightOffset_sec = 0;

// --- PINES ESP32-S3 ---
#define OLED_SDA 8      // Pines Pantalla
#define OLED_SCL 9
#define ENCODER_CLK 10  // Pines ENCODER
#define ENCODER_DT 11   
#define ENCODER_SW 12   
#define PIN_SENSOR 4    // Sensor de humedad
#define PIN_BOMBA 13    // Relé Bomba
#define PIN_LUZ 14      // Relé Luz

// --- CALIBRACIÓN SENSOR ---
const int VALOR_SECO = 3000; 
const int VALOR_AGUA = 1000; 

// --- OBJETOS ---
Adafruit_SSD1306 display(128, 64, &Wire, -1);
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ENCODER_CLK, ENCODER_DT, ENCODER_SW, -1, 4);
WebServer servidorAPI(80);

// --- VARIABLES GLOBALES ---
int lecturaRaw = 0;           // lectura adc
int valorHumedad = 0;         
bool estadoBomba = false;
bool estadoLuz = false;
bool modoAutoBomba = false;   // false = manual, true = auto
bool modoAutoLuz = false; 
int umbralHumedad = 40;       // humedad necesaria para encender la bomba
int hLuzOn = 18, mLuzOn = 0;  // variables para la base de datos
int hLuzOff = 22, mLuzOff = 0; 

bool flagCambioLocal = false; // Cambiar el tiempo
unsigned long t_sensor = 0, t_interaccion = 0, t_firebase = 0, t_pantalla = 0; // Diferentes tiempos

enum Pantalla { INICIO, MENU, SUB_HUM, SUB_BOMBA, SUB_LUZ, SUB_RED }; // Estados de la pantalla
Pantalla pantallaActual = INICIO;
int opcionSel = 0; 

String itemsMenu[] = {"1. Ver Humedad", "2. Control Bomba", "3. Control Luz", "4. Estado WiFi", "5. <- Volver"};
String itemsSub[] = {"  Encender", "  Apagar", "  Volver"};

void IRAM_ATTR readEncoderISR() { rotaryEncoder.readEncoder_ISR(); }

// --- FUNCIONES CORE ---
void actualizarHardware() {
  digitalWrite(PIN_BOMBA, estadoBomba ? HIGH : LOW);
  digitalWrite(PIN_LUZ, estadoLuz ? HIGH : LOW);
}

// Función para extraer números de Firebase
int extraerValorJSON(String payload, String clave) {
  int inicio = payload.indexOf("\"" + clave + "\":");
  if (inicio == -1) return -1;
  inicio += clave.length() + 3;
  int finComa = payload.indexOf(",", inicio);
  int finLlave = payload.indexOf("}", inicio);
  int fin = (finComa != -1 && finComa < finLlave) ? finComa : finLlave;
  return payload.substring(inicio, fin).toInt();
}

// Función para enviar y recibir datos de la Firebase
void sincronizarFirebase(bool forzarEnvioLocal = false) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    // 1. RECIBIR DATOS (Si no es un forzado local)
    if (!forzarEnvioLocal) {
      http.begin(client, FIREBASE_URL);
      if (http.GET() > 0) {
        String p = http.getString();
        
        if (p.indexOf("\"autoB\":true") > 0) modoAutoBomba = true;
        else if (p.indexOf("\"autoB\":false") > 0) modoAutoBomba = false;

        if (p.indexOf("\"autoL\":true") > 0) modoAutoLuz = true;
        else if (p.indexOf("\"autoL\":false") > 0) modoAutoLuz = false;
        
        if(!modoAutoBomba){
           if (p.indexOf("\"bom\":true") > 0) estadoBomba = true;
           else if (p.indexOf("\"bom\":false") > 0) estadoBomba = false;
        }
        if(!modoAutoLuz){
           if (p.indexOf("\"luz\":true") > 0) estadoLuz = true;
           else if (p.indexOf("\"luz\":false") > 0) estadoLuz = false;
        }
        
        int u = extraerValorJSON(p, "umb");      if(u != -1) umbralHumedad = u;
        int hOn = extraerValorJSON(p, "hLuzOn"); if(hOn != -1) hLuzOn = hOn;
        int mOn = extraerValorJSON(p, "mLuzOn"); if(mOn != -1) mLuzOn = mOn;
        int hOff = extraerValorJSON(p, "hLuzOff"); if(hOff != -1) hLuzOff = hOff;
        int mOff = extraerValorJSON(p, "mLuzOff"); if(mOff != -1) mLuzOff = mOff;
        
        actualizarHardware();
      }
      http.end();
    }

    // 2. ENVIAR DATOS
    http.begin(client, FIREBASE_URL);
    http.addHeader("Content-Type", "application/json");
    String json = "{\"hum\":" + String(valorHumedad) + ",\"bom\":" + (estadoBomba?"true":"false") + 
                  ",\"luz\":" + (estadoLuz?"true":"false") + ",\"autoB\":" + (modoAutoBomba?"true":"false") + 
                  ",\"autoL\":" + (modoAutoLuz?"true":"false") + ",\"umb\":" + String(umbralHumedad) + 
                  ",\"hLuzOn\":" + String(hLuzOn) + ",\"mLuzOn\":" + String(mLuzOn) +
                  ",\"hLuzOff\":" + String(hLuzOff) + ",\"mLuzOff\":" + String(mLuzOff) + "}";
    http.PATCH(json);
    http.end();
  }
}

void centrarTexto(String texto, int y, int tamano) {
  display.setTextSize(tamano);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(texto, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, y);
  display.print(texto);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOMBA, OUTPUT); 
  pinMode(PIN_LUZ, OUTPUT);
  actualizarHardware();

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000); 
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED no encontrada");
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextColor(WHITE);
  centrarTexto("SISTEMA SMART", 25, 1);
  display.display();

  WiFiManager wm;
  wm.setConnectTimeout(10);     // Tiene 10s para conectarse
  wm.setConfigPortalTimeout(120);  // Si no se conecta en 10s, se activa el punto de acceso para configurar el wifi
  wm.autoConnect("Macetero_Smart");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  servidorAPI.on("/datos", []() {
    String json = "{\"hum\":" + String(valorHumedad) + ",\"bom\":" + String(estadoBomba) + ",\"luz\":" + String(estadoLuz) + "}";
    servidorAPI.send(200, "application/json", json);
  });
  servidorAPI.begin();

  pinMode(ENCODER_SW, INPUT_PULLUP); 
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 0, false);
}

void loop() {
  if(WiFi.status() == WL_CONNECTED) servidorAPI.handleClient();

  // Leer sensor y ejecutar (cada 1s)
  if(millis() - t_sensor > 1000) {
    t_sensor = millis();
    lecturaRaw = analogRead(PIN_SENSOR);
    valorHumedad = map(lecturaRaw, VALOR_SECO, VALOR_AGUA, 0, 100);
    valorHumedad = constrain(valorHumedad, 0, 100);

    // Lógica Automática Bomba
    if (modoAutoBomba) {
      if (valorHumedad < umbralHumedad) estadoBomba = true;
      else if (valorHumedad > umbralHumedad + 5) estadoBomba = false; 
    }

    // Lógica Automática Luz
    if (modoAutoLuz) {
      struct tm timeinfo;
      if(getLocalTime(&timeinfo, 0)){ 
        // Obtener el tiempo
        int horaActualMinutos = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
        int inicioMinutos = (hLuzOn * 60) + mLuzOn;
        int finMinutos = (hLuzOff * 60) + mLuzOff;
        bool deberiaEstarEncendida = false;

        // Ver si ya esta en el tiempo de encender y de apagar
        if (inicioMinutos < finMinutos) deberiaEstarEncendida = (horaActualMinutos >= inicioMinutos && horaActualMinutos < finMinutos);
        else if (inicioMinutos > finMinutos) deberiaEstarEncendida = (horaActualMinutos >= inicioMinutos || horaActualMinutos < finMinutos);

        if (estadoLuz != deberiaEstarEncendida) {
          estadoLuz = deberiaEstarEncendida;
          flagCambioLocal = true; 
        }
      }
    }
    actualizarHardware();
  }

  // Sincronizar Firebase 
  if (flagCambioLocal) { sincronizarFirebase(true); flagCambioLocal = false; t_firebase = millis(); }
  else if (millis() - t_firebase > 10000) { t_firebase = millis(); sincronizarFirebase(false); }

  // Navegación Encoder
  if (rotaryEncoder.encoderChanged()) {
    t_interaccion = millis();
    if(pantallaActual == INICIO) {
      pantallaActual = MENU;
      rotaryEncoder.setBoundaries(0, 4, true);
      rotaryEncoder.setEncoderValue(0);
    } 
    else { 
      opcionSel = rotaryEncoder.readEncoder(); 
    }
  }

  // Lógica Botón Encoder
  if (digitalRead(ENCODER_SW) == LOW) {
    t_interaccion = millis();
    delay(250); 
    if (pantallaActual == INICIO) { 
      pantallaActual = MENU; 
      rotaryEncoder.setBoundaries(0, 4, true); 
    } 
    else if (pantallaActual == MENU) {
      if (opcionSel == 0) pantallaActual = SUB_HUM;
      if (opcionSel == 1) { pantallaActual = SUB_BOMBA; rotaryEncoder.setEncoderValue(0); opcionSel = 0; }
      if (opcionSel == 2) { pantallaActual = SUB_LUZ; rotaryEncoder.setEncoderValue(0); opcionSel = 0; }
      if (opcionSel == 3) pantallaActual = SUB_RED;
      if (opcionSel == 4) pantallaActual = INICIO;
    } else if (pantallaActual == SUB_BOMBA && !modoAutoBomba) { 
      if (opcionSel == 0) { estadoBomba = true; actualizarHardware(); flagCambioLocal = true; }
      if (opcionSel == 1) { estadoBomba = false; actualizarHardware(); flagCambioLocal = true; }
      if (opcionSel == 2) { pantallaActual = MENU; rotaryEncoder.setBoundaries(0, 4, true); rotaryEncoder.setEncoderValue(1); opcionSel = 1; }
    } else if (pantallaActual == SUB_LUZ && !modoAutoLuz) { 
      if (opcionSel == 0) { estadoLuz = true; actualizarHardware(); flagCambioLocal = true; }
      if (opcionSel == 1) { estadoLuz = false; actualizarHardware(); flagCambioLocal = true; }
      if (opcionSel == 2) { pantallaActual = MENU; rotaryEncoder.setBoundaries(0, 4, true); rotaryEncoder.setEncoderValue(2); opcionSel = 2; }
    } else { 
      pantallaActual = MENU; 
      rotaryEncoder.setBoundaries(0, 4, true); }
  }

  // Retorno automático a Inicio
  if(pantallaActual != INICIO && (millis() - t_interaccion > 15000)) {
    pantallaActual = INICIO;
    rotaryEncoder.setBoundaries(0, 0, false);
  }

  // Refresco de pantalla controlado 
  if (millis() - t_pantalla > 200) {
    t_pantalla = millis();
    dibujarOLED();
  }
}

void dibujarOLED() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  
  if(pantallaActual == INICIO) {
    display.setTextSize(1);
    display.setCursor(0, 0); display.print("WiFi:" + String(WiFi.status()==WL_CONNECTED?"OK":"--"));
    
    // Reloj en esquina superior
    display.setCursor(95, 0);
    struct tm timeinfo;
    if(getLocalTime(&timeinfo, 0)) {
      String hStr = (timeinfo.tm_hour < 10 ? "0" : "") + String(timeinfo.tm_hour);
      String mStr = (timeinfo.tm_min < 10 ? "0" : "") + String(timeinfo.tm_min);
      display.print(hStr + ":" + mStr);
    } else {
      display.print("--:--");
    }

    display.setCursor(60, 0); display.print(estadoBomba ? "ON" : "OFF");
    centrarTexto(String(valorHumedad) + "%", 22, 3);
    display.drawFastHLine(0, 52, 128, WHITE);
    centrarTexto("Gira para Menu >", 56, 1);
  } 
  else if(pantallaActual == MENU) {
    centrarTexto("MENU PRINCIPAL", 0, 1);
    display.drawFastHLine(0, 9, 128, WHITE);
    for(int i=0; i<5; i++) {
      display.setCursor(0, 14 + (i * 10));
      display.print((i == opcionSel ? "> " : "  ") + itemsMenu[i]);
    }
  } 
  else if(pantallaActual == SUB_BOMBA || pantallaActual == SUB_LUZ) {
    display.setTextSize(1);
    display.setCursor(0,0); 
    display.print(pantallaActual == SUB_BOMBA ? "--- BOMBA ---" : "--- LUZ RGB ---");
    display.setCursor(100, 0);
    bool est = (pantallaActual == SUB_BOMBA) ? estadoBomba : estadoLuz;
    display.print(est ? "ON" : "OFF");

    for(int i=0; i<3; i++) {
      display.setCursor(15, 22 + (i * 13)); 
      display.print((i == opcionSel ? "> " : "  ") + itemsSub[i]);
    }
  }
  else if(pantallaActual == SUB_HUM) {
    centrarTexto("HUMEDAD", 0, 1);
    centrarTexto(String(valorHumedad) + "%", 20, 3);
    display.setTextSize(1);
    display.setCursor(0, 52); display.print("RAW: "); display.print(lecturaRaw);
    display.setCursor(80, 52); display.print("V:3.3");
  }
  else if(pantallaActual == SUB_RED) {
    centrarTexto("INFO RED", 0, 1);
    display.setTextSize(1);
    display.setCursor(0, 20); display.print("SSID: " + WiFi.SSID());
    display.setCursor(0, 35); display.print("IP: " + WiFi.localIP().toString());
    centrarTexto("Click volver", 56, 1);
  }
  
  display.display();
}