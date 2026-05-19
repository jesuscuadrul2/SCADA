#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include "time.h"
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Adafruit_PN532.h>
#include <Adafruit_INA219.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Encoder.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ==========================================
// 1. MAPEO DE PINES 
// ==========================================
#define TFT_CS    10
#define TFT_MOSI  11
#define TFT_SCK   12
#define TFT_RST   18
#define TFT_DC    46

#define SDA_PIN   8
#define SCL_PIN   9

#define ENC_CLK   4
#define ENC_DT    5
#define ENC_SW    6

#define PIN_RELE_1 7
#define PIN_RELE_2 14 
#define PIN_RELE_3 16
#define PIN_RELE_4 17
const int pinesRele[4] = {PIN_RELE_1, PIN_RELE_2, PIN_RELE_3, PIN_RELE_4};

#define PIN_BUZZER 47
#define BUZZER_CANAL 0
#define PIN_NEOPIXEL 48

#define DIP_SELLADO  2
#define DIP_MATERIA  42
#define DIP_SERVIDOR 21

#define PN532_IRQ   38
#define PN532_RESET 39

// ==========================================
// 2. RED Y SERVIDOR
// ==========================================
const char* ssid = "INFINITUM8DFF_2.4";
const char* password = "6uqRe52JsV";
const char* mqtt_server = "192.168.1.200"; 

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ==========================================
// 3. OBJETOS Y CONSTANTES
// ==========================================
Adafruit_GC9A01A tft = Adafruit_GC9A01A(TFT_CS, TFT_DC, TFT_RST);
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
Adafruit_INA219 ina1(0x40); 
Adafruit_INA219 ina2(0x44); 
Adafruit_NeoPixel rgbLed(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
ESP32Encoder encoder;

#define CX 120
#define CY 120
#define BG_BLACK      0x0000
#define TEXT_COLOR    0xFFFF
#define HIGHLIGHT     0x03DF
#define COLOR_GREEN   0x07E0
#define COLOR_RED     0xF800
#define COLOR_YELLOW  0xFFE0
#define COLOR_DARK    0x4A49

// ==========================================
// 4. MÁQUINA DE ESTADOS Y VARIABLES
// ==========================================
enum EstadoSistema { SALVAPANTALLAS, TELEMETRIA, CONTROL_CARGAS, ESPERANDO_GERENTE, CONFIRMAR_OVERRIDE, ALERTA_INTRUSO, MENU_ADMIN, ESPERANDO_TAG_NUEVO };
EstadoSistema estadoActual = SALVAPANTALLAS;
bool forzarRedibujo = true; 

enum Rol { DESCONOCIDO = 0, OPERADOR = 1, TECNICO = 2, GERENTE = 3 };
Rol usuarioActivo = DESCONOCIDO; 
String uidActivoStr = "SISTEMA";
String nombreActivo = "";

int cargaSeleccionada = 0; 
int opcionAdmin = 1; 
bool estadoRele[4] = {false, false, false, false};
unsigned long tiempoEncendido[4] = {0, 0, 0, 0}; 

unsigned long ultimoTiempoInteraccion = 0;
int ultimoPasoEncoder = 0;

bool bloqueoPorError = false; 
bool overrideGerente = false; 
bool sirenaEncendida = false;
String motivoBloqueo = ""; 

float v1=0, mA1=0, mW1=0; float v2=0, mA2=0, mW2=0;
unsigned long ultimoMuestreo = 0; 
unsigned long ultimoNeoPixel = 0; 
unsigned long ultimoEscaneoNFC = 0; 

// ==========================================
// 5. FUNCIONES DE APOYO Y MQTT
// ==========================================
void hacerBeep(int freq, int dur) { 
  ledcWriteTone(BUZZER_CANAL, freq); delay(dur); ledcWriteTone(BUZZER_CANAL, 0); 
}

String getUIDString(uint8_t* uid, uint8_t len) {
  String result = "";
  for (int i=0; i<len; i++) { if(uid[i] < 0x10) result += "0"; result += String(uid[i], HEX); }
  result.toUpperCase(); return result;
}

void enviarLog(String evento, int nivel) {
  if (!mqtt.connected()) return;
  StaticJsonDocument<200> doc;
  doc["uid"] = uidActivoStr; doc["evento"] = evento; doc["nivel"] = nivel;
  char jsonBuffer[200]; serializeJson(doc, jsonBuffer);
  mqtt.publish("jarvis/scada/log", jsonBuffer);
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String msg; for (int i = 0; i < length; i++) msg += (char)payload[i];
  String tpc = String(topic);

  if (tpc == "jarvis/scada/auth/response") {
    StaticJsonDocument<200> doc; deserializeJson(doc, msg);
    int rolLeido = doc["rol"];
    nombreActivo = doc["nombre"].as<String>();
    
    if (rolLeido == 0) {
      estadoActual = ALERTA_INTRUSO; forzarRedibujo = true;
      enviarLog("Intento de acceso denegado", 3);
    } else {
      if (estadoActual == ALERTA_INTRUSO) { ledcWriteTone(BUZZER_CANAL, 0); sirenaEncendida = false; }
      usuarioActivo = (Rol)rolLeido;
      hacerBeep(1200, 100);
      enviarLog("Acceso concedido a " + nombreActivo, 1);
      
      if (usuarioActivo == GERENTE && estadoActual != SALVAPANTALLAS && bloqueoPorError) {
          estadoActual = CONFIRMAR_OVERRIDE; forzarRedibujo = true;
      } else {
          estadoActual = TELEMETRIA; forzarRedibujo = true;
      }
    }
  }
}

void reconectarMQTT() {
  if (!mqtt.connected()) {
    if (mqtt.connect("SCADA_ESP32_V7")) mqtt.subscribe("jarvis/scada/auth/response");
  }
}

void actualizarNeoPixel(bool hayFalloFisico, int cargasActivas) {
  if (millis() - ultimoNeoPixel < 250) return; 
  ultimoNeoPixel = millis();

  if (estadoActual == ALERTA_INTRUSO) {
    if ((millis() / 250) % 2 == 0) rgbLed.setPixelColor(0, rgbLed.Color(255, 0, 0));
    else rgbLed.setPixelColor(0, rgbLed.Color(0, 0, 255));
  } 
  else if (hayFalloFisico || bloqueoPorError) {
    rgbLed.setPixelColor(0, rgbLed.Color(150, 0, 0)); 
  } 
  else if (mA1 >= 350 || mA2 >= 80) { 
    rgbLed.setPixelColor(0, rgbLed.Color(255, 100, 0)); 
  } 
  else if (cargasActivas == 4) {
    rgbLed.setPixelColor(0, rgbLed.Color(0, 150, 0)); 
  } 
  else if (cargasActivas > 0) {
    rgbLed.setPixelColor(0, rgbLed.Color(150, 150, 0)); 
  } 
  else {
    rgbLed.setPixelColor(0, rgbLed.Color(0, 0, 0)); 
  }
  rgbLed.show();
}

// ==========================================
// 6. INTERFAZ GRÁFICA
// ==========================================
void mostrarMensaje(String linea1, String linea2, uint16_t color) {
  tft.fillScreen(color); tft.setTextColor(TEXT_COLOR); tft.setTextSize(2); 
  tft.setCursor(15, 90); tft.print(linea1);
  tft.setTextSize(1); tft.setCursor(20, 130); tft.print(linea2); 
  forzarRedibujo = true; 
}

void dibujarArcoSolido(int x, int y, int r, float startAngle, float endAngle, uint16_t color, int grosor) {
  float startRad = startAngle * PI / 180.0; float endRad = endAngle * PI / 180.0;
  for (float a = startRad; a <= endRad; a += 0.03) tft.fillCircle(x + r * cos(a), y + r * sin(a), grosor / 2, color);
}

void renderizarSalvapantallas() {
  if (forzarRedibujo) {
    tft.fillScreen(BG_BLACK);
    tft.drawCircle(CX, CY, 115, COLOR_DARK);
    tft.setTextSize(2); tft.setTextColor(COLOR_GREEN);
    tft.setCursor(45, 50); tft.print("SCADA OS 3.0"); 
    tft.setTextSize(1); tft.setCursor(55, 170); tft.print("STANDBY - LOCKED");
    forzarRedibujo = false;
  }
  struct tm timeinfo;
  if(getLocalTime(&timeinfo, 10)) {
    tft.setTextSize(4); tft.setTextColor(TEXT_COLOR, BG_BLACK);
    tft.setCursor(25, 100); tft.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
}

void renderizarMenuAdmin() {
  if (forzarRedibujo) {
    tft.fillScreen(0x0000); tft.drawCircle(CX, CY, 115, 0x03DF);
    tft.setTextSize(2); tft.setTextColor(0xFFFF); tft.setCursor(35, 40); tft.print("ADMIN TAGS");
    
    String opcionStr = ""; uint16_t col = 0xFFFF;
    if (opcionAdmin == 1) { opcionStr = "+ OPERADOR"; col = 0xFFE0; }
    else if (opcionAdmin == 2) { opcionStr = "+ TECNICO"; col = 0x07E0; }
    else if (opcionAdmin == 3) { opcionStr = "+ GERENTE"; col = 0x03DF; }
    else if (opcionAdmin == 4) { opcionStr = "- ELIMINAR"; col = 0xF800; }
    else if (opcionAdmin == 5) { opcionStr = "< SALIR"; col = 0x4A49; }

    tft.setTextSize(3); tft.setTextColor(col); tft.setCursor(30, 100); tft.print(opcionStr);
    tft.setTextSize(1); tft.setTextColor(0x4A49); tft.setCursor(65, 180); tft.print("Click = Seleccionar");
    forzarRedibujo = false;
  }
}

void renderizarTelemetria() {
  if (forzarRedibujo) {
    tft.fillScreen(BG_BLACK);
    tft.setTextSize(2); tft.setTextColor(TEXT_COLOR);
    tft.setCursor(60, 30); tft.print("CARGA "); tft.print(cargaSeleccionada + 1);

    tft.setTextSize(1); tft.setTextColor(COLOR_DARK); 
    tft.setCursor(75, 80); tft.print("VOLTS"); tft.setCursor(135, 80); tft.print("mAMPS");
    tft.setCursor(105, 140); tft.print("POWER");

    dibujarArcoSolido(CX, CY, 110, 115, 245, COLOR_DARK, 6); 
    dibujarArcoSolido(CX, CY, 110, -65, 65, COLOR_DARK, 6);  
    forzarRedibujo = false;
  }

  float v = 0, mA = 0, mW = 0; bool tieneSensor = false;
  if (cargaSeleccionada == 0) { v = v1; mA = mA1; mW = mW1; tieneSensor = true; }
  else if (cargaSeleccionada == 1) { v = v2; mA = mA2; mW = mW2; tieneSensor = true; }

  if (tieneSensor) {
    tft.setTextSize(2); 
    tft.setTextColor(COLOR_GREEN, BG_BLACK); tft.setCursor(55, 95); tft.printf("%5.1f", v);
    tft.setTextColor(COLOR_YELLOW, BG_BLACK); tft.setCursor(120, 95); tft.printf("%5.1f", mA);
    tft.setTextColor(COLOR_RED, BG_BLACK); tft.setCursor(85, 155); tft.printf("%5.1fW", mW / 1000.0);
  } else {
    tft.setTextSize(2); tft.setTextColor(COLOR_DARK, BG_BLACK);
    tft.setCursor(55, 95); tft.print(" 0.0 "); tft.setCursor(120, 95); tft.print("  0.0");
    tft.setCursor(85, 155); tft.print(" NO SENSOR");
  }
}

void renderizarControl() {
  if (forzarRedibujo) {
    tft.fillScreen(BG_BLACK);
    tft.drawCircle(CX, CY, 115, HIGHLIGHT); tft.drawCircle(CX, CY, 116, HIGHLIGHT);
    tft.setTextSize(2); tft.setTextColor(TEXT_COLOR); tft.setCursor(65, 45); tft.print("CONTROL");
    
    tft.setTextColor(HIGHLIGHT); tft.setCursor(70, 90); tft.print("CARGA "); tft.print(cargaSeleccionada + 1);
    tft.setTextSize(1); tft.setTextColor(COLOR_DARK); tft.setCursor(95, 190); tft.print("Click = Toggle");
    forzarRedibujo = false;
  }

  bool encendida = estadoRele[cargaSeleccionada];
  tft.setTextSize(4); 
  tft.setTextColor(encendida ? COLOR_GREEN : COLOR_RED, BG_BLACK);
  tft.setCursor(85, 130); tft.print(encendida ? " ON  " : " OFF "); 
}

// ==========================================
// 7. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  for(int i=0; i<4; i++) { pinMode(pinesRele[i], OUTPUT); digitalWrite(pinesRele[i], HIGH); }

  pinMode(DIP_SELLADO, INPUT_PULLUP); pinMode(DIP_MATERIA, INPUT_PULLUP); pinMode(DIP_SERVIDOR, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT); ledcSetup(BUZZER_CANAL, 2000, 8); ledcAttachPin(PIN_BUZZER, BUZZER_CANAL);
  pinMode(ENC_SW, INPUT_PULLUP); encoder.attachHalfQuad(ENC_CLK, ENC_DT);

  rgbLed.begin(); rgbLed.setBrightness(40); 
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS); tft.begin(); tft.setRotation(0);
  
  mostrarMensaje("WIFI SYNC", "Conectando...", COLOR_DARK);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  configTime(-21600, 0, "pool.ntp.org"); 

  mqtt.setServer(mqtt_server, 1884); 
  mqtt.setCallback(callbackMQTT);

  Wire.begin(SDA_PIN, SCL_PIN);
  nfc.begin(); nfc.SAMConfig();
  ina1.begin(); ina2.begin(); 
  
  estadoActual = SALVAPANTALLAS; forzarRedibujo = true; ultimoTiempoInteraccion = millis();
}

// ==========================================
// 8. LOOP PRINCIPAL
// ==========================================
void loop() {
  unsigned long ahora = millis();
  if (WiFi.status() == WL_CONNECTED) reconectarMQTT();
  mqtt.loop();

  if (ahora - ultimoMuestreo > 333) {
    v1 = ina1.getBusVoltage_V(); mA1 = ina1.getCurrent_mA() * 0.983; 
    if (mA1 < 2.0 && mA1 > -2.0) mA1 = 0; mW1 = v1 * mA1;

    v2 = ina2.getBusVoltage_V(); mA2 = ina2.getCurrent_mA() * 0.983; 
    if (mA2 < 2.0 && mA2 > -2.0) mA2 = 0; mW2 = v2 * mA2;
    
    ultimoMuestreo = ahora;

    if (mqtt.connected()) {
      StaticJsonDocument<256> tel;
      tel["v1"] = v1; tel["ma1"] = mA1; tel["mw1"] = mW1;
      tel["v2"] = v2; tel["ma2"] = mA2; tel["mw2"] = mW2;
      char out[256]; serializeJson(tel, out); mqtt.publish("jarvis/scada/telemetry", out);
    }
    if(estadoActual == TELEMETRIA) renderizarTelemetria(); else if(estadoActual == SALVAPANTALLAS) renderizarSalvapantallas();
  }

  // 2. LÓGICA DE FALLOS Y SOBRECORRIENTE
  bool hayFalloFisico = (digitalRead(DIP_SELLADO) == HIGH) || (digitalRead(DIP_MATERIA) == HIGH) || (digitalRead(DIP_SERVIDOR) == HIGH);
  
  bool ignorarInrushC1 = (estadoRele[0] && (ahora - tiempoEncendido[0] < 4000));
  bool ignorarInrushC2 = (estadoRele[1] && (ahora - tiempoEncendido[1] < 4000));

  bool sobreCorrienteC1 = (!ignorarInrushC1 && mA1 > 400.0);
  bool sobreCorrienteC2 = (!ignorarInrushC2 && mA2 > 100.0);

  int cargasActivas = 0; for(int i=0; i<4; i++) if(estadoRele[i]) cargasActivas++;

  if (((hayFalloFisico && cargasActivas > 0 && !overrideGerente) || sobreCorrienteC1 || sobreCorrienteC2) && !bloqueoPorError) {
      for(int i=0; i<4; i++) { estadoRele[i] = false; digitalWrite(pinesRele[i], HIGH); }
      bloqueoPorError = true; 
      overrideGerente = false; 
      cargasActivas = 0;
      
      
      if (sobreCorrienteC1) {
          motivoBloqueo = "Carga 1 > 400mA";
          mostrarMensaje("SOBRE CORRIENTE", motivoBloqueo, COLOR_RED);
          hacerBeep(2000, 1000); 
          enviarLog("Disparo: Sobrecorriente L1", 3);
      } else if (sobreCorrienteC2) {
          motivoBloqueo = "Carga 2 > 100mA";
          mostrarMensaje("SOBRE CORRIENTE", motivoBloqueo, COLOR_RED);
          hacerBeep(2000, 1000); 
          enviarLog("Disparo: Sobrecorriente L2", 3);
      } else if (digitalRead(DIP_SELLADO) == HIGH) {
          motivoBloqueo = "Compuerta Abierta";
          mostrarMensaje("FALLO FISICO", motivoBloqueo, COLOR_RED);
          enviarLog("Paro: Compuerta Sellado", 3);
      } else if (digitalRead(DIP_MATERIA) == HIGH) {
          motivoBloqueo = "Falta Materia Prima";
          mostrarMensaje("FALLO FISICO", motivoBloqueo, COLOR_RED);
          enviarLog("Paro: Falta Materia", 3);
      } else if (digitalRead(DIP_SERVIDOR) == HIGH) {
          motivoBloqueo = "Servidor Offline";
          mostrarMensaje("FALLO FISICO", motivoBloqueo, COLOR_RED);
          enviarLog("Paro: Servidor Desconectado", 3);
      }
      delay(2000);
      estadoActual = TELEMETRIA; forzarRedibujo = true;
  }

  actualizarNeoPixel(hayFalloFisico, cargasActivas);

  if (estadoActual == ALERTA_INTRUSO) {
    if ((ahora % 600) < 300) { if (!sirenaEncendida) { ledcWriteTone(BUZZER_CANAL, 2000); sirenaEncendida = true; } } 
    else { if (sirenaEncendida) { ledcWriteTone(BUZZER_CANAL, 0); sirenaEncendida = false; } }
  } else { if (sirenaEncendida) { ledcWriteTone(BUZZER_CANAL, 0); sirenaEncendida = false; } }

  if (estadoActual != ALERTA_INTRUSO && estadoActual != SALVAPANTALLAS) {
    if (ahora - ultimoTiempoInteraccion > 20000) {
      if(usuarioActivo != DESCONOCIDO) enviarLog("Sesión cerrada por inactividad", 1);
      estadoActual = SALVAPANTALLAS; usuarioActivo = DESCONOCIDO; uidActivoStr = "SISTEMA"; forzarRedibujo = true;
    }
  }

  int pasos = encoder.getCount() / 2;
  if (pasos != ultimoPasoEncoder && estadoActual != ALERTA_INTRUSO && estadoActual != SALVAPANTALLAS && estadoActual != ESPERANDO_TAG_NUEVO) {
    ultimoTiempoInteraccion = ahora;
    if (estadoActual == MENU_ADMIN) {
        if (pasos > ultimoPasoEncoder) opcionAdmin++; else opcionAdmin--;
        if (opcionAdmin > 5) opcionAdmin = 1; if (opcionAdmin < 1) opcionAdmin = 5;
        forzarRedibujo = true; renderizarMenuAdmin();
    } else {
        if (pasos > ultimoPasoEncoder) cargaSeleccionada++; else cargaSeleccionada--;
        int maxIndex = (usuarioActivo == GERENTE) ? 4 : 3;
        if (cargaSeleccionada > maxIndex) cargaSeleccionada = 0; if (cargaSeleccionada < 0) cargaSeleccionada = maxIndex;
        if (cargaSeleccionada == 4) { estadoActual = MENU_ADMIN; opcionAdmin = 1; forzarRedibujo = true; renderizarMenuAdmin(); }
        else { estadoActual = TELEMETRIA; forzarRedibujo = true; renderizarTelemetria(); }
    }
    ultimoPasoEncoder = pasos;
  }

  if (digitalRead(ENC_SW) == LOW && estadoActual != ALERTA_INTRUSO && estadoActual != SALVAPANTALLAS) {
    delay(50); 
    if (digitalRead(ENC_SW) == LOW) {
      ultimoTiempoInteraccion = ahora;
      
      if (estadoActual == MENU_ADMIN) {
         if (opcionAdmin == 5) { estadoActual = TELEMETRIA; cargaSeleccionada = 0; forzarRedibujo = true; }
         else { estadoActual = ESPERANDO_TAG_NUEVO; mostrarMensaje("ADMIN", "ACERQUE NUEVA TARJETA", 0x03DF); }
      }
      else if (estadoActual == TELEMETRIA) {
        if (bloqueoPorError) {
            if (usuarioActivo == GERENTE) {
                bloqueoPorError = false; 
                overrideGerente = hayFalloFisico; 
                mostrarMensaje("SISTEMA RESTAURADO", "Sesion Finalizada", COLOR_GREEN);
                enviarLog("Gerente levanto bloqueo", 2);
                delay(2000);


                usuarioActivo = DESCONOCIDO;
                uidActivoStr = "SISTEMA";
                estadoActual = SALVAPANTALLAS;
                forzarRedibujo = true;
            } else {
                mostrarMensaje("BLOQUEO ACTIVO", "Requiere Gerencia", 0xF800); 
                delay(1500); forzarRedibujo = true;
            }
        } else {
            if (usuarioActivo == GERENTE || usuarioActivo == TECNICO) {
                estadoActual = CONTROL_CARGAS; forzarRedibujo = true; renderizarControl(); 
            } else { hacerBeep(500, 200); }
        }
      }
      else if (estadoActual == CONTROL_CARGAS) {
        if (usuarioActivo == GERENTE || usuarioActivo == TECNICO) {
          estadoRele[cargaSeleccionada] = !estadoRele[cargaSeleccionada];
          if (estadoRele[cargaSeleccionada]) tiempoEncendido[cargaSeleccionada] = millis(); 

          digitalWrite(pinesRele[cargaSeleccionada], !estadoRele[cargaSeleccionada]);
          hacerBeep(2500, 100); renderizarControl();
          enviarLog(estadoRele[cargaSeleccionada] ? "Encendio Carga " + String(cargaSeleccionada+1) : "Apago Carga " + String(cargaSeleccionada+1), 1);
        }
      }
      else if (estadoActual == CONFIRMAR_OVERRIDE) {
        bloqueoPorError = false; 
        overrideGerente = hayFalloFisico; 
        mostrarMensaje("SISTEMA RESTAURADO", "Sesion Finalizada", COLOR_GREEN);
        enviarLog("Bloqueo Liberado por Gerencia", 2);
        delay(2000);

        
        usuarioActivo = DESCONOCIDO;
        uidActivoStr = "SISTEMA";
        estadoActual = SALVAPANTALLAS; 
        forzarRedibujo = true; 
      }
      while(digitalRead(ENC_SW) == LOW); 
    }
  }

  if (ahora - ultimoEscaneoNFC > 300) { 
    ultimoEscaneoNFC = ahora;
    uint8_t uid[7]; uint8_t uidLength;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 40)) {
      ultimoTiempoInteraccion = ahora; 
      String scannedUID = getUIDString(uid, uidLength);

      if (estadoActual == ESPERANDO_TAG_NUEVO) {
         hacerBeep(1800, 150);
         StaticJsonDocument<200> doc; doc["uid"] = scannedUID;
         if (opcionAdmin < 4) { 
           doc["rol"] = opcionAdmin; char out[200]; serializeJson(doc, out); mqtt.publish("jarvis/scada/admin/add", out); 
         } else { 
           char out[200]; serializeJson(doc, out); mqtt.publish("jarvis/scada/admin/remove", out); 
         }
         mostrarMensaje("EXITO", "Servidor Actualizado", 0x07E0); delay(1500);
         estadoActual = MENU_ADMIN; forzarRedibujo = true; renderizarMenuAdmin();
      } 
      else {
         mostrarMensaje("SISTEMA", "Verificando Red...", 0x4A49);
         uidActivoStr = scannedUID;
         StaticJsonDocument<200> doc; doc["uid"] = scannedUID;
         char out[200]; serializeJson(doc, out);
         mqtt.publish("jarvis/scada/auth/request", out);
      }
      delay(1000); 
    }
  }
}
