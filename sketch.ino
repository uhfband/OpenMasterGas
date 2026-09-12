#define WM_NODEBUG

#define BUTTON_PIN 13
#define RX_PIN 12
#define REMOTE_ENABLE_PIN 5
#define REMOTE_RX_PIN 4

#define JSON_BUFFER_SIZE 200

#define WIFI_CONNECT_TIMEOUT 10000

#define DEEP_SLEEP_TIME 1800e6  //30 min

#define SLEEP_TIME_MINUTE 60000000

#include <ESP8266WiFi.h>
#include <WiFiManager.h> 
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>

ADC_MODE(ADC_VCC);

WiFiClientSecure wifiClient;
WiFiManager wifiManager;
ESP8266WebServer server(80);

SoftwareSerial rx_boiler(RX_PIN, -1);
SoftwareSerial rx_remote(REMOTE_RX_PIN, -1, true);

char buf[7];

byte raw_data1[4];
byte raw_data2[24];

//from boiler
int coolant_temperature = 0;
int burner = 0;
int water_temperature = 0;  

//from remote
int remote_target_temp = 50;
int remote_power = 1;
int remote_air_temp = 24;

//override
int power = 0;
int target_temp = 50;

int passthrough = 1;

void handleRoot() {
  StaticJsonDocument<JSON_BUFFER_SIZE> doc;

  //boiler state
  doc["burner"] = burner;
  doc["coolant_t"] = coolant_temperature;

  //remote state
  doc["r_target_t"] = remote_target_temp;
  doc["r_power"] = remote_power;
  doc["r_air_t"] = remote_air_temp;

  //override
  doc["passthrough"] = passthrough;
  if (!passthrough) {
      doc["o_power"] = power;
      doc["o_target_t"] = target_temp;
  }

  String msg;
  serializeJson(doc, msg);
  server.send(200, "application/json", msg);
}

void handleRaw() {
  StaticJsonDocument<JSON_BUFFER_SIZE> doc;
  char str[49];
  byte offset;
  byte b;
  char hex_digits[] = "0123456789ABCDEF";
  
  sprintf(str,"%02X%02X%02X%02X", raw_data1[0], raw_data1[1], raw_data1[2], raw_data1[3]);
  doc["data1"] = str;

  memset(str, 0, sizeof(str));
  for (int n=0; n<24; n++) {
      b = raw_data2[n];
      str[n*2] = hex_digits[b>>4];
      str[n*2 + 1] = hex_digits[b&0x0F];
  }

  doc["data2"] = str;

  doc["RSSI"] = WiFi.RSSI();
  doc["Vdd"] = ESP.getVcc()/1000.00;

  String msg;
  serializeJson(doc, msg);
  server.send(200, "application/json", msg);
}

void handleSet() {
  for ( uint8_t i = 0; i < server.args(); i++ ) {
    if (server.argName(i) == "temperature") {
      target_temp = server.arg(i).toInt();
      passthrough = 0;
    }
    if (server.argName(i) == "power") {
      power = server.arg(i).toInt();
      passthrough = 0;
    }
    if (server.argName(i) == "passthrough") {
      passthrough = server.arg(i).toInt();
    }
  }
  
  server.send(200, "text/plain", "OK");
}

void setup() {

  pinMode(BUTTON_PIN, INPUT);
  //pinMode(TX_PIN, OUTPUT);
  pinMode(REMOTE_ENABLE_PIN, OUTPUT);
  digitalWrite(REMOTE_ENABLE_PIN, HIGH);

  //tx serial
  Serial1.begin(4800, SERIAL_8N1, SERIAL_FULL, 1, true);
  
  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  Serial.setTimeout(2000);

  if (ESP.getResetInfoPtr()->reason == REASON_DEFAULT_RST) {
    if (digitalRead(BUTTON_PIN)) {
      delay(1000);
      
      Serial.println("start ConfigPortal");
      wifiManager.startConfigPortal();
      
      Serial.println("restarting");
      delay(1000);
      ESP.restart();
    }
  }

  String ssid = wifiManager.getWiFiSSID();
  //String wifi_password = wifiManager.getWiFiPass();

  Serial.print("\r\n");
  Serial.println("Connecting to " + ssid);
  
  int started = millis();

  WiFi.mode(WIFI_STA);
  WiFi.begin();

  while (WiFi.status() != WL_CONNECTED) {
    delay(50);
    Serial.print(".");

    if ((millis() - started) > WIFI_CONNECT_TIMEOUT) {
      //Serial.print("\r\nConnect timeout! Going to sleep...");
      //ESP.deepSleep(5 * SLEEP_TIME_MINUTE);
      Serial.println("\r\nConnect timeout!");
      passthrough = 1;
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    int connect_time = millis() - started;
     // Debugging - Output the IP Address of the ESP8266
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    server.on("/", handleRoot);
    server.on("/set", handleSet);
    server.on("/raw", handleRaw);

    server.begin();
    Serial.println("HTTP server started"); 
  }

  //rx serials
  rx_boiler.begin(4800);
  rx_remote.begin(4800);
  
}

void clear_buffer(SoftwareSerial& serial)
{
  char c;
  while (serial.available() > 0) {
      c = serial.read();  
  }
}

bool wait_packet(SoftwareSerial& serial, char *buf)
{
    if (serial.available() < 7) {
      return false;  
    }
    
    char c;
    int n=0;

    c = serial.read();

    if (c != 0xF9 && c != 0xFA) {
      return false;
    }
    buf[0] = c;

    for (n = 1; n<7; n++) {
      buf[n] = serial.read();
    }

    //check crc
    if (buf[6] == calc_crc(buf)) {
        return true;  
    } else {
        //crc_e_cnt++;  
    }
    
    return false;
}

void parse_packet(const char* buf) {
    int val = 0;
    byte n;
    byte offset;

    if (buf[0] == 0xFA) { //boiler state unknown
        offset = buf[1];
        for (n = 0; n<4; n++) {
            raw_data2[offset + n] = buf[2 + n];  
        }
        return;  
    }
    
    if (buf[0] == 0xF9 && buf[1] == 0x00) { //boiler state
        coolant_temperature = buf[3];
        burner = (buf[2]>>6) & 1;
        water_temperature = buf[4];

        for (n = 0; n<4; n++) {
            raw_data1[n] = buf[2 + n];  
        }
        return;
    }

    if (buf[0] == 0xF9 && buf[1] == 0x80) { //remote state
        val = buf[2] & 0x01;
        if (val != remote_power) {
            passthrough = 1;
            remote_power = val;  
        }
        
        val = buf[3];
        if (val != remote_target_temp) {
            passthrough = 1;
            remote_target_temp = val;
        }

        remote_air_temp = buf[5];
    }
}

bool is_boiler(const char* buf)
{
    return (buf[0] == 0xFA) || (buf[0] == 0xF9 && buf[1] == 0x00);
}

char calc_crc(const char* buf) {
    char sum = 0;
    for (int n=0; n<6; n++) {
      sum += buf[n];
    }
    return (sum^0xFF) + 1;
}

byte reply[7] = {0xf9, 0x80, 0x01, 0x3e, 0x28, 0x18, 0x08};

void send_reply() {
    reply[2] = power;
    reply[3] = target_temp;
    reply[6] = calc_crc((const char*)reply);
    Serial1.write(reply, 7);
}

void print_packet(const char* buf) {
  byte b;
  for (int n=0; n<7; n++) {
        b = buf[n];
        Serial.print(b>>4,  HEX);
        Serial.print(b&0x0F, HEX);
      }
      Serial.print("\n");
}

void loop() { 
  server.handleClient();
  
  if (wait_packet(rx_boiler, buf)) {
      //print_packet(buf);

      if (passthrough) {
          parse_packet(buf);  
          return;
      }
      
      if (is_boiler(buf)) {
          parse_packet(buf);
          
          digitalWrite(REMOTE_ENABLE_PIN, LOW);
          
          if (buf[0] == 0xF9) {
              delay(4);
              send_reply();
              delay(30);
          } else {    //0xFA
              clear_buffer(rx_remote);
              rx_remote.enableRx(true);
              delay(34);
              rx_remote.enableRx(false);
              if (wait_packet(rx_remote, buf)) {
                  parse_packet(buf);
              }
          }
          
          digitalWrite(REMOTE_ENABLE_PIN, HIGH);
      }
   }
}
