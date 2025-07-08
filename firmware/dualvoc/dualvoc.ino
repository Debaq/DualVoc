#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// Configuración de pines
#define BUTTON_PIN 0
#define LED_PIN 1
#define OLED_SDA 3
#define OLED_SCL 33

// Configuración OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Configuración de cámara para ESP32-S
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Variables globales
WebServer server(80);
bool ledState = false;
bool sdCardPresent = false;
String ssid = "";
String password = "";
bool isAPMode = false;
unsigned long lastButtonPress = 0;
int buttonPressCount = 0;
bool buttonPressed = false;
bool longPress = false;
int photoCount = 0;

void setup() {
  // Configurar pines
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED apagado inicialmente (HIGH = apagado para pin 1)
  
  // Inicializar I2C para OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  
  // Inicializar OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // Si falla OLED, no hay forma de mostrar error
    while(1);
  }
  
  showStatus("DualVOC", "Iniciando...", "");
  delay(1000);
  
  // Inicializar SD
  if(!SD_MMC.begin()) {
    showError("Error SD Card");
    sdCardPresent = false;
    // Continuar sin SD
  } else {
    sdCardPresent = true;
    showStatus("SD OK", "Configurando...", "");
  }
  
  // Configurar cámara
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA; // 800x600 para stream
  config.jpeg_quality = 12;
  config.fb_count = 1;
  
  if(esp_camera_init(&config) != ESP_OK) {
    showError("Error Camara");
    while(1);
  }
  
  showStatus("Camara OK", "Config WiFi...", "");
  
  // Cargar configuración WiFi
  loadWiFiConfig();
  
  // Configurar WiFi
  setupWiFi();
  
  // Configurar servidor web
  setupWebServer();
  
  // Crear directorio de imagenes si no existe
  if(sdCardPresent && !SD_MMC.exists("/imagenes")) {
    SD_MMC.mkdir("/imagenes");
  }
  
  // Contar imagenes existentes
  if(sdCardPresent) {
    countPhotos();
  }
  
  showStatus("DualVOC listo", "", "");
  delay(1000);
  updateDisplay();
}

void loop() {
  server.handleClient();
  handleButton();
  delay(10);
}

void handleButton() {
  bool currentState = digitalRead(BUTTON_PIN) == LOW;
  
  if(currentState && !buttonPressed) {
    buttonPressed = true;
    lastButtonPress = millis();
    buttonPressCount++;
  }
  
  if(!currentState && buttonPressed) {
    buttonPressed = false;
    unsigned long pressDuration = millis() - lastButtonPress;
    
    if(pressDuration > 1000) { // Pulsación larga
      longPress = true;
      ledState = false;
      digitalWrite(LED_PIN, HIGH); // Apagar LED (HIGH = apagado)
      showStatus("LED:", "Apagado", "(Mantener)");
      delay(1000);
      updateDisplay();
      buttonPressCount = 0;
    }
  }
  
  // Verificar pulsaciones múltiples
  if(buttonPressCount > 0 && (millis() - lastButtonPress > 500)) {
    if(!longPress) {
      if(buttonPressCount == 1) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? LOW : HIGH); // LOW = encendido, HIGH = apagado
        showStatus("LED:", ledState ? "Encendido" : "Apagado", "");
        delay(1000);
        updateDisplay();
      }
      else if(buttonPressCount >= 2) {
        takePhoto();
      }
    }
    buttonPressCount = 0;
    longPress = false;
  }
}

void takePhoto() {
  if(!sdCardPresent) {
    showStatus("Error:", "No hay SD", "");
    delay(2000);
    updateDisplay();
    return;
  }
  
  showStatus("Capturando...", "imagen medica", "");
  
  // Cambiar resolución para foto
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_UXGA); // 1600x1200 para fotos
  
  camera_fb_t * fb = esp_camera_fb_get();
  if(!fb) {
    showStatus("Error:", "Captura fallo", "");
    s->set_framesize(s, FRAMESIZE_SVGA); // Volver a resolución de stream
    delay(2000);
    updateDisplay();
    return;
  }
  
  photoCount++;
  String filename = "/imagenes/imagen_" + String(photoCount) + ".jpg";
  
  File file = SD_MMC.open(filename, FILE_WRITE);
  if(!file) {
    showStatus("Error:", "Guardando", "");
    esp_camera_fb_return(fb);
    s->set_framesize(s, FRAMESIZE_SVGA);
    delay(2000);
    updateDisplay();
    return;
  }
  
  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);
  
  // Volver a resolución de stream
  s->set_framesize(s, FRAMESIZE_SVGA);
  
  showStatus("Imagen " + String(photoCount), "guardada OK", "");
  delay(1500);
  
  // Volver a mostrar IP
  updateDisplay();
}

void loadWiFiConfig() {
  if(!sdCardPresent) return;
  
  File file = SD_MMC.open("/config.json", FILE_READ);
  if(!file) {
    return;
  }
  
  String content = file.readString();
  file.close();
  
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, content);
  
  ssid = doc["ssid"].as<String>();
  password = doc["password"].as<String>();
}

void saveWiFiConfig() {
  if(!sdCardPresent) return;
  
  DynamicJsonDocument doc(1024);
  doc["ssid"] = ssid;
  doc["password"] = password;
  
  File file = SD_MMC.open("/config.json", FILE_WRITE);
  if(file) {
    serializeJson(doc, file);
    file.close();
  }
}

void setupWiFi() {
  if(ssid.length() == 0) {
    startAPMode();
  } else {
    connectToWiFi();
  }
}

void startAPMode() {
  isAPMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("DualVOC-Setup", "");
  
  IPAddress ip = WiFi.softAPIP();
  updateDisplay("Modo AP", ip.toString(), "DualVOC-Setup");
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  showStatus("Conectando...", ssid.substring(0, 12), "");
  
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    attempts++;
    if(attempts % 5 == 0) {
      showStatus("Conectando...", ssid.substring(0, 8), String(attempts) + "/30");
    }
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    isAPMode = false;
    IPAddress ip = WiFi.localIP();
    updateDisplay("Conectado", ip.toString(), ssid.substring(0, 12));
  } else {
    startAPMode();
  }
}

void setupWebServer() {
  // Página principal
  server.on("/", HTTP_GET, []() {
    if(sdCardPresent) {
      File file = SD_MMC.open("/index.html", FILE_READ);
      if(file) {
        server.streamFile(file, "text/html");
        file.close();
      } else {
        // HTML básico embebido si no hay archivo en SD
        String html = "<!DOCTYPE html><html><head><title>DualVOC - Otoscopio Educativo</title></head><body>";
        html += "<h1>DualVOC</h1><h2>Otoscopio Educativo Digital</h2>";
        html += "<img src='/stream' style='max-width:100%'><br>";
        html += "<a href='/gallery'>Galeria de Imagenes</a> | <a href='/config'>Configuracion</a>";
        html += "</body></html>";
        server.send(200, "text/html", html);
      }
    } else {
      // HTML básico cuando no hay SD
      String html = "<!DOCTYPE html><html><head><title>DualVOC - Otoscopio Educativo</title></head><body>";
      html += "<h1>DualVOC (Sin SD)</h1><h2>Otoscopio Educativo Digital</h2>";
      html += "<img src='/stream' style='max-width:100%'><br>";
      html += "<a href='/config'>Configuracion WiFi</a>";
      html += "<p>Error: SD Card no presente - No se pueden guardar imagenes</p>";
      html += "</body></html>";
      server.send(200, "text/html", html);
    }
  });
  
  // Stream de cámara
  server.on("/stream", HTTP_GET, []() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "multipart/x-mixed-replace; boundary=frame");
    
    while(true) {
      camera_fb_t * fb = esp_camera_fb_get();
      if(!fb) break;
      
      server.sendContent("--frame\r\n");
      server.sendContent("Content-Type: image/jpeg\r\n");
      server.sendContent("Content-Length: " + String(fb->len) + "\r\n\r\n");
      server.sendContent((char*)fb->buf, fb->len);
      server.sendContent("\r\n");
      
      esp_camera_fb_return(fb);
      
      if(!server.client().connected()) break;
    }
  });
  
  // Galería de imágenes
  server.on("/gallery", HTTP_GET, []() {
    if(!sdCardPresent) {
      server.send(500, "text/html", "<h1>Error: SD Card no presente</h1>");
      return;
    }
    
    File file = SD_MMC.open("/gallery.html", FILE_READ);
    if(file) {
      server.streamFile(file, "text/html");
      file.close();
    } else {
      String html = "<html><body><h1>Galería de Imágenes Médicas</h1>";
      File root = SD_MMC.open("/imagenes");
      File imageFile = root.openNextFile();
      
      while(imageFile) {
        if(!imageFile.isDirectory()) {
          String filename = imageFile.name();
          html += "<div><img src='/photo/" + filename + "' style='max-width:300px;'>";
          html += "<br>" + filename + "</div><br>";
        }
        imageFile = root.openNextFile();
      }
      
      html += "<a href='/'>Volver</a></body></html>";
      server.send(200, "text/html", html);
    }
  });
  
  // Servir imágenes
  server.on("/photo", HTTP_GET, []() {
    String path = server.uri();
    if(path.startsWith("/photo/")) {
      String filename = path.substring(7); // Remover "/photo/"
      File file = SD_MMC.open("/imagenes/" + filename, FILE_READ);
      if(!file) {
        server.send(404, "text/plain", "Imagen no encontrada");
        return;
      }
      server.streamFile(file, "image/jpeg");
      file.close();
    } else {
      String filename = server.arg("file");
      if(filename.length() == 0) {
        server.send(400, "text/plain", "Archivo no especificado");
        return;
      }
      
      File file = SD_MMC.open("/imagenes/" + filename, FILE_READ);
      if(!file) {
        server.send(404, "text/plain", "Imagen no encontrada");
        return;
      }
      
      server.streamFile(file, "image/jpeg");
      file.close();
    }
  });
  
  // Configuración
  server.on("/config", HTTP_GET, []() {
    if(sdCardPresent) {
      File file = SD_MMC.open("/config.html", FILE_READ);
      if(file) {
        server.streamFile(file, "text/html");
        file.close();
      } else {
        String html = "<html><body><h1>Configuración DualVOC</h1>";
        html += "<form action='/api/wifi' method='post'>";
        html += "SSID: <input type='text' name='ssid' value='" + ssid + "'><br>";
        html += "Password: <input type='password' name='password' value='" + password + "'><br>";
        html += "<input type='submit' value='Guardar'>";
        html += "</form></body></html>";
        server.send(200, "text/html", html);
      }
    } else {
      String html = "<html><body><h1>Configuración DualVOC (Sin SD)</h1>";
      html += "<form action='/api/wifi' method='post'>";
      html += "SSID: <input type='text' name='ssid'><br>";
      html += "Password: <input type='password' name='password'><br>";
      html += "<input type='submit' value='Guardar'>";
      html += "</form></body></html>";
      server.send(200, "text/html", html);
    }
  });
  
  // API para obtener lista de imágenes
  server.on("/api/photos", HTTP_GET, []() {
    if(!sdCardPresent) {
      server.send(500, "application/json", "{\"error\":\"SD Card no presente\"}");
      return;
    }
    
    String json = "{\"photos\":[";
    File root = SD_MMC.open("/imagenes");
    if(!root) {
      server.send(500, "application/json", "{\"error\":\"Error accediendo directorio imagenes\"}");
      return;
    }
    
    File file = root.openNextFile();
    bool first = true;
    
    while(file) {
      if(!file.isDirectory()) {
        if(!first) json += ",";
        json += "{\"name\":\"";
        json += String(file.name());
        json += "\",\"size\":";
        json += String(file.size());
        json += ",\"date\":\"";
        json += getFileDate(file);
        json += "\"}";
        first = false;
      }
      file = root.openNextFile();
    }
    
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  // API para eliminar imagen
  server.on("/api/delete", HTTP_POST, []() {
    if(!sdCardPresent) {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"SD Card no presente\"}");
      return;
    }
    
    String body = server.arg("plain");
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, body);
    
    String filename = doc["filename"];
    if(filename.length() == 0) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Nombre de archivo requerido\"}");
      return;
    }
    
    if(SD_MMC.remove("/imagenes/" + filename)) {
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Error eliminando imagen\"}");
    }
  });
  
  // API para estado del sistema
  server.on("/api/status", HTTP_GET, []() {
    String json = "{";
    json += "\"wifiStatus\":\"";
    json += (WiFi.status() == WL_CONNECTED ? "Conectado" : "Desconectado");
    json += "\",\"ipAddress\":\"";
    json += (isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
    json += "\",\"wifiSignal\":\"";
    json += String(WiFi.RSSI());
    json += " dBm\",\"sdSpace\":\"";
    json += getSDSpace();
    json += "\",\"uptime\":\"";
    json += getUptime();
    json += "\",\"freeMemory\":\"";
    json += String(ESP.getFreeHeap() / 1024);
    json += " KB\",\"totalPhotos\":";
    json += String(photoCount);
    json += ",\"lastPhoto\":\"";
    json += getLastPhotoName();
    json += "\"}";
    server.send(200, "application/json", json);
  });
  
  // API para escanear redes WiFi
  server.on("/api/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    
    for(int i = 0; i < n; i++) {
      if(i > 0) json += ",";
      json += "{\"ssid\":\"";
      json += WiFi.SSID(i);
      json += "\",\"rssi\":";
      json += String(WiFi.RSSI(i));
      json += ",\"authMode\":";
      json += String(WiFi.encryptionType(i));
      json += "}";
    }
    
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  // API para configurar WiFi
  server.on("/api/wifi", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    
    if(newSSID.length() == 0) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"SSID requerido\"}");
      return;
    }
    
    ssid = newSSID;
    password = newPassword;
    saveWiFiConfig();
    
    server.send(200, "application/json", "{\"success\":true}");
    delay(1000);
    ESP.restart();
  });
  
  // API para configuración de cámara
  server.on("/api/camera", HTTP_POST, []() {
    sensor_t * s = esp_camera_sensor_get();
    if(!s) {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Sensor no disponible\"}");
      return;
    }
    
    String resolution = server.arg("resolution");
    String quality = server.arg("quality");
    String brightness = server.arg("brightness");
    String contrast = server.arg("contrast");
    
    // Aplicar configuraciones
    if(quality.length() > 0) {
      s->set_quality(s, quality.toInt());
    }
    if(brightness.length() > 0) {
      s->set_brightness(s, brightness.toInt());
    }
    if(contrast.length() > 0) {
      s->set_contrast(s, contrast.toInt());
    }
    
    // Guardar configuración en SD
    saveCameraConfig();
    
    server.send(200, "application/json", "{\"success\":true}");
  });
  
  // API para obtener configuración de cámara
  server.on("/api/camera-settings", HTTP_GET, []() {
    String json = "{";
    json += "\"resolution\":\"UXGA\",";
    json += "\"quality\":12,";
    json += "\"brightness\":0,";
    json += "\"contrast\":0";
    json += "}";
    server.send(200, "application/json", json);
  });
  
  // API para reiniciar WiFi
  server.on("/api/reset-wifi", HTTP_POST, []() {
    WiFi.disconnect();
    delay(1000);
    connectToWiFi();
    server.send(200, "application/json", "{\"success\":true}");
  });
  
  // API para limpiar configuración WiFi
  server.on("/api/clear-wifi", HTTP_POST, []() {
    ssid = "";
    password = "";
    if(sdCardPresent && SD_MMC.exists("/config.json")) {
      SD_MMC.remove("/config.json");
    }
    server.send(200, "application/json", "{\"success\":true}");
  });
  
  // API para reiniciar sistema
  server.on("/api/restart", HTTP_POST, []() {
    server.send(200, "application/json", "{\"success\":true}");
    delay(1000);
    ESP.restart();
  });
  
  // API para reset de fábrica
  server.on("/api/factory-reset", HTTP_POST, []() {
    if(sdCardPresent) {
      // Eliminar configuración
      if(SD_MMC.exists("/config.json")) SD_MMC.remove("/config.json");
      
      // Eliminar todas las imágenes
      File root = SD_MMC.open("/imagenes");
      File file = root.openNextFile();
      while(file) {
        if(!file.isDirectory()) {
          String filename = String(file.name());
          if(SD_MMC.remove("/imagenes/" + filename)) {
            // Imagen eliminada
          }
        }
        file = root.openNextFile();
      }
    }
    
    server.send(200, "application/json", "{\"success\":true}");
    delay(1000);
    ESP.restart();
  });
  
  // Servir archivos estáticos desde SD
  server.onNotFound([]() {
    String path = server.uri();
    
    // Si es una ruta de archivo, intentar servirlo desde SD
    if(sdCardPresent && (path.endsWith(".html") || path.endsWith(".css") || path.endsWith(".js"))) {
      File file = SD_MMC.open(path, FILE_READ);
      if(file) {
        String contentType = "text/html";
        if(path.endsWith(".css")) contentType = "text/css";
        else if(path.endsWith(".js")) contentType = "application/javascript";
        
        server.streamFile(file, contentType);
        file.close();
        return;
      }
    }
    
    server.send(404, "text/plain", "Archivo no encontrado");
  });

  server.begin();
}

void updateDisplay(String line1, String line2, String line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, 10);
  display.println(line2);
  display.setCursor(0, 20);
  display.println(line3);
  display.display();
}

void updateDisplay() {
  // Mostrar información actual del sistema
  if(WiFi.status() == WL_CONNECTED) {
    updateDisplay("Conectado", WiFi.localIP().toString(), ssid.substring(0, 12));
  } else if(isAPMode) {
    updateDisplay("Modo AP", WiFi.softAPIP().toString(), "DualVOC-Setup");
  } else {
    updateDisplay("Desconectado", "", "");
  }
}

void showStatus(String line1, String line2, String line3) {
  updateDisplay(line1, line2, line3);
}

void showError(String error) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ERROR:");
  display.setCursor(0, 10);
  display.println(error);
  display.setCursor(0, 20);
  if(!sdCardPresent) {
    display.println("Sin SD Card");
  }
  display.display();
}

void countPhotos() {
  if(!sdCardPresent) return;
  
  File root = SD_MMC.open("/imagenes");
  if(!root) return;
  
  File file = root.openNextFile();
  int count = 0;
  
  while(file) {
    if(!file.isDirectory()) {
      count++;
    }
    file = root.openNextFile();
  }
  
  photoCount = count;
}

String getFileDate(File file) {
  // Retorna fecha ficticia ya que ESP32 no mantiene timestamps reales sin RTC
  return "2024-01-01 12:00:00";
}

String getSDSpace() {
  if(!sdCardPresent) return "N/A";
  
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  uint64_t usedBytes = SD_MMC.usedBytes() / (1024 * 1024);
  
  String result = String((int)(cardSize - usedBytes));
  result += " MB libre / ";
  result += String((int)cardSize);
  result += " MB total";
  return result;
}

String getUptime() {
  unsigned long uptime = millis() / 1000;
  unsigned long days = uptime / 86400;
  uptime %= 86400;
  unsigned long hours = uptime / 3600;
  uptime %= 3600;
  unsigned long minutes = uptime / 60;
  
  String result = String((int)days);
  result += "d ";
  result += String((int)hours);
  result += "h ";
  result += String((int)minutes);
  result += "m";
  return result;
}

String getLastPhotoName() {
  if(photoCount > 0) {
    return "imagen_" + String(photoCount) + ".jpg";
  }
  return "Ninguna";
}

void saveCameraConfig() {
  if(!sdCardPresent) return;
  
  DynamicJsonDocument doc(1024);
  doc["resolution"] = "UXGA";
  doc["quality"] = 12;
  doc["brightness"] = 0;
  doc["contrast"] = 0;
  
  File file = SD_MMC.open("/camera_config.json", FILE_WRITE);
  if(file) {
    serializeJson(doc, file);
    file.close();
  }
}