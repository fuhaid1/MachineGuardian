/*
 * Machine Guardian — ESP32 Security System Firmware
 * SCT-013 100A/50mA + PN532 NFC + SSD1306 OLED + Active Buzzer
 *
 * PCB: See wiring diagram. Star ground, 100nF decoupling per module.
 *      CT cable: twisted pair ≤15cm, shield grounded at PCB end only.
 * Telegram: set TELEGRAM_TOKEN + TELEGRAM_CHAT_ID, leave blank to disable.
 * Libraries: Adafruit PN532, SSD1306, GFX, ArduinoJson (others built-in).
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_PN532.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <math.h>
#include <time.h>

// ─── Wi-Fi ────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ─── Telegram ─────────────────────────────────────────────────
// Set to "" to disable. See setup guide in header above.
const char* TELEGRAM_TOKEN   = "";  // paste your bot token here, leave blank to disable
const char* TELEGRAM_CHAT_ID = "";  // paste your Telegram chat ID here
const char* TELEGRAM_HOST    = "api.telegram.org";

// ─── Pins ─────────────────────────────────────────────────────
const int ADC_PIN    = 34;
const int GREEN_LED  = 18;
const int RED_LED    = 19;
const int YELLOW_LED = 5;
const int BUZZER_PIN = 23;

// Sensor constants — CT ratio and burden determine the A/V conversion factor.
const float BURDEN_OHM  = 33.0f;
const float TURNS_RATIO = 2000.0f;

// Detection threshold in net amps (above ambient). Persisted in NVS, editable via Settings.
float gThreshold = 1.0f;

// gAmbientAmps: raw RMS baseline captured with machine off. Subtracted from every reading.
// gFilteredNet: EMA-smoothed net amps (after ambient subtraction). Used for all decisions.
// gMidpoint: ADC DC midpoint (~1834 at 3.3V/2). Drifts slowly with temperature.
float gAmbientAmps = 0.0f;
const float LP_ALPHA = 0.15f;
float gFilteredNet  = 0.0f;
float gMidpoint     = 1834.6f;

// ─── NVS ──────────────────────────────────────────────────────
Preferences prefs;

// ─── OLED ─────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET   -1
#define OLED_ADDR    0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── NFC ──────────────────────────────────────────────────────
Adafruit_PN532 nfc(21, 22);

// ─── Web server ───────────────────────────────────────────────
WebServer server(80);

// ─── Employees (NVS persisted) ────────────────────────────────
#define MAX_EMPLOYEES 20
struct Employee { char uid[20]; char name[64]; bool active; };
Employee employees[MAX_EMPLOYEES];
int      employeeCount = 0;

// Loads employees, ambient baseline, and threshold from NVS into RAM.
void nvsLoadEmployees() {
  prefs.begin("guardian", false);
  employeeCount = prefs.getInt("emp_count", 0);
  for (int i = 0; i < employeeCount && i < MAX_EMPLOYEES; i++) {
    char ku[16],kn[16],ka[16];
    snprintf(ku,16,"emp_uid_%d",i); snprintf(kn,16,"emp_name_%d",i); snprintf(ka,16,"emp_act_%d",i);
    prefs.getString(ku, employees[i].uid,  19);
    prefs.getString(kn, employees[i].name, 63);
    employees[i].active = prefs.getUChar(ka,1)!=0;
  }
  gAmbientAmps = prefs.getFloat("ambient",   0.0f);
  gThreshold   = prefs.getFloat("threshold", 1.0f);
  prefs.end();
  Serial.printf("[NVS] %d employees, ambient=%.3fA, threshold=%.2fA\n", employeeCount, gAmbientAmps, gThreshold);
}

// Writes all employee records to NVS. Called after every add/remove/toggle.
void nvsSaveEmployees() {
  prefs.begin("guardian", false);
  prefs.putInt("emp_count", employeeCount);
  for (int i = 0; i < employeeCount; i++) {
    char ku[16],kn[16],ka[16];
    snprintf(ku,16,"emp_uid_%d",i); snprintf(kn,16,"emp_name_%d",i); snprintf(ka,16,"emp_act_%d",i);
    prefs.putString(ku, employees[i].uid);
    prefs.putString(kn, employees[i].name);
    prefs.putUChar(ka,  employees[i].active?1:0);
  }
  prefs.end();
}

// Persists ambient baseline and threshold to NVS.
void nvsSaveCalibration() {
  prefs.begin("guardian",false);
  prefs.putFloat("ambient",   gAmbientAmps);
  prefs.putFloat("threshold", gThreshold);
  prefs.end();
}

bool isAuthorised(const String& uid, String& outName) {
  for (int i=0;i<employeeCount;i++) {
    if (employees[i].active && uid==String(employees[i].uid)) { outName=String(employees[i].name); return true; }
  }
  return false;
}

// ─── System state ─────────────────────────────────────────────
enum MachineState { STATE_IDLE, STATE_AUTHENTICATED, STATE_RUNNING, STATE_UNAUTHORIZED };
MachineState  currentState     = STATE_IDLE;
bool          machineOn        = false;
String        currentEmployee  = "";
String        currentUID       = "";
unsigned long authTime         = 0;
unsigned long machineStartTime = 0;
const unsigned long AUTH_TIMEOUT_MS = 30000;

// ─── Data storage ─────────────────────────────────────────────
#define MAX_SAMPLES 288
struct PowerSample  { unsigned long ts; float amps; float watts; };
struct RuntimeEntry {
  char date[11]; char employee[64]; unsigned long seconds;
  bool unauthorized;   // true = machine ran without auth
};
struct NotifEntry { unsigned long ts; char message[128]; char type[16]; };

PowerSample  powerLog[MAX_SAMPLES];
int          powerLogHead=0, powerLogCount=0;
RuntimeEntry runtimeLog[100];
int          runtimeLogCount=0;
NotifEntry   notifLog[50];
int          notifHead=0, notifCount=0;

unsigned long lastPowerSample=0;
const unsigned long POWER_SAMPLE_INTERVAL=5000;

// ─── Helpers ──────────────────────────────────────────────────

void addNotification(const char* msg, const char* type) {
  int idx=notifHead%50;
  notifLog[idx].ts=time(nullptr);
  strncpy(notifLog[idx].message,msg,127); notifLog[idx].message[127]='\0';
  strncpy(notifLog[idx].type,   type,15); notifLog[idx].type[15]='\0';
  notifHead++; if(notifCount<50)notifCount++;
  Serial.printf("[NOTIF][%s] %s\n",type,msg);
}

String uidToString(uint8_t* uid, uint8_t len) {
  if(!uid||len==0||len>7) return "";
  String s="";
  for(uint8_t i=0;i<len;i++){if(uid[i]<0x10)s+="0"; s+=String(uid[i],HEX);}
  s.toUpperCase(); return s;
}

String getDateString() {
  time_t now=time(nullptr); struct tm* t=localtime(&now);
  char buf[11]; snprintf(buf,11,"%04d-%02d-%02d",t->tm_year+1900,t->tm_mon+1,t->tm_mday);
  return String(buf);
}

// Records runtime — employee name is "UNAUTHORIZED" for unauth sessions
void recordRuntime(const String& emp, unsigned long sec, bool unauth=false) {
  String today=getDateString();
  for(int i=0;i<runtimeLogCount;i++) {
    if(String(runtimeLog[i].date)==today && String(runtimeLog[i].employee)==emp
       && runtimeLog[i].unauthorized==unauth)
      { runtimeLog[i].seconds+=sec; return; }
  }
  if(runtimeLogCount<100) {
    strncpy(runtimeLog[runtimeLogCount].date,    today.c_str(),10);
    strncpy(runtimeLog[runtimeLogCount].employee,emp.c_str(),  63);
    runtimeLog[runtimeLogCount].seconds=sec;
    runtimeLog[runtimeLogCount].unauthorized=unauth;
    runtimeLogCount++;
  }
}

// ─── Telegram ─────────────────────────────────────────────────
// Runs entirely on the ESP32 using WiFiClientSecure.
// HTTPS connection to api.telegram.org:443.
// certificate verification is skipped (setInsecure) — acceptable
// for this use case as the token itself provides authentication.

void sendTelegram(const char* text) {
  if(strlen(TELEGRAM_TOKEN)==0 || strlen(TELEGRAM_CHAT_ID)==0) return;
  if(WiFi.status()!=WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();  
  client.setTimeout(8000);

  if(!client.connect(TELEGRAM_HOST, 443)) {
    Serial.println("[TG] Connection failed");
    return;
  }

  
  String msg = String(text);
  msg.replace("\"","\\\"");

  String body = "{\"chat_id\":\"" + String(TELEGRAM_CHAT_ID) +
                "\",\"text\":\"" + msg +
                "\",\"parse_mode\":\"HTML\"}";

  String path = "/bot" + String(TELEGRAM_TOKEN) + "/sendMessage";

  client.println("POST " + path + " HTTP/1.1");
  client.println("Host: " + String(TELEGRAM_HOST));
  client.println("Content-Type: application/json");
  client.println("Content-Length: " + String(body.length()));
  client.println("Connection: close");
  client.println();
  client.print(body);

  
  unsigned long t=millis();
  while(client.connected() && millis()-t<5000) {
    if(client.available()) {
      String line=client.readStringUntil('\n');
      if(line.startsWith("HTTP/")) Serial.println("[TG] "+line);
      if(line=="\r") break;
    }
  }
  client.stop();
}

// ─── OLED ─────────────────────────────────────────────────────
void displayMessage(const char* l1, const char* l2="", const char* l3="") {
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
  display.setCursor(0,0);  display.println(l1);
  if(l2[0]){display.setCursor(0,12);display.println(l2);}
  if(l3[0]){display.setCursor(0,24);display.println(l3);}
  display.display();
}

// ─── Buzzer ───────────────────────────────────────────────────
void beepShort(){digitalWrite(BUZZER_PIN,HIGH);delay(100);digitalWrite(BUZZER_PIN,LOW);}
void beepAlarm(){
  for(int i=0;i<6;i++){
    digitalWrite(RED_LED,HIGH);digitalWrite(BUZZER_PIN,HIGH);delay(150);
    digitalWrite(RED_LED,LOW); digitalWrite(BUZZER_PIN,LOW); delay(100);
  }
}

// ─── LEDs ─────────────────────────────────────────────────────
void setLEDs(bool r,bool y,bool g){
  digitalWrite(RED_LED,   r?HIGH:LOW);
  digitalWrite(YELLOW_LED,y?HIGH:LOW);
  digitalWrite(GREEN_LED, g?HIGH:LOW);
}

// ─── ADC measurement (I2C paused during sampling) ─────────────
float measureRawRMS() {
  Wire.end();  
  long sumSq=0,sumRaw=0;
  for(int i=0;i<2000;i++){
    int raw=analogRead(ADC_PIN);
    sumRaw+=raw;
    int off=raw-(int)gMidpoint;
    sumSq+=(long)off*off;
    delayMicroseconds(50);
  }
  Wire.begin(21,22);  
  float newMid=(float)sumRaw/2000.0f;
  gMidpoint=gMidpoint*0.97f+newMid*0.03f;
  float rmsADC=sqrtf((float)sumSq/2000.0f);
  float rmsV=rmsADC*(3.3f/4095.0f);
  return (rmsV/BURDEN_OHM)*TURNS_RATIO;
}

float measureNetAmps() {
  float raw=measureRawRMS();
  float net=raw-gAmbientAmps;
  if(net<0.0f) net=0.0f;
  if(net<0.3f) net=0.0f;
  gFilteredNet=LP_ALPHA*net+(1.0f-LP_ALPHA)*gFilteredNet;
  return gFilteredNet;
}

// Takes 30 raw RMS samples with machine off, averages them, adds 10% margin, saves to NVS.
float captureAmbient() {
  displayMessage("Measuring ambient","Machine must be OFF","Do not move clamp");
  float sum=0;
  for(int i=0;i<30;i++){sum+=measureRawRMS();delay(100);}
  float avg=sum/30.0f;
  gAmbientAmps=avg*1.1f;
  nvsSaveCalibration();
  gFilteredNet=0.0f;
  Serial.printf("[AMB] ambient=%.3fA\n",gAmbientAmps);
  return gAmbientAmps;
}

// ─── Unauthorized session tracking ────────────────────────────
bool          unauthRunning    = false;
unsigned long unauthStartTime  = 0;

void startUnauthSession() {
  unauthRunning   = true;
  unauthStartTime = millis();
}

void stopUnauthSession() {
  if(!unauthRunning) return;
  unsigned long sec=(millis()-unauthStartTime)/1000;
  if(sec>0) recordRuntime("UNAUTHORIZED",sec,true);
  unauthRunning=false;
}

// ─── State machine ────────────────────────────────────────────
void enterIdle() {
  currentState=STATE_IDLE; currentEmployee=""; currentUID="";
  setLEDs(true,false,false);
  displayMessage("Authentication","Required","Scan your card");
}

void enterAuthenticated(const String& emp) {
  currentState=STATE_AUTHENTICATED; currentEmployee=emp; authTime=millis();
  setLEDs(false,true,false);
  char l2[64]; snprintf(l2,64,"Welcome %s",emp.c_str());
  displayMessage("Machine Ready",l2,"Start the machine");
  beepShort();
  char notif[128]; snprintf(notif,128,"%s authenticated",emp.c_str());
  addNotification(notif,"info");
}

void enterRunning(float amps) {
  currentState=STATE_RUNNING; machineStartTime=millis();
  setLEDs(false,false,true);
  char l2[32]; snprintf(l2,32,"%.1fA / %.0fW",amps,amps*220.0f);
  displayMessage("Machine Running",l2,currentEmployee.c_str());
  char notif[128]; snprintf(notif,128,"Machine started by %s",currentEmployee.c_str());
  addNotification(notif,"info");
}

void enterUnauthorized() {
  currentState=STATE_UNAUTHORIZED;
  setLEDs(false,false,false);
  displayMessage("! UNAUTHORIZED !","Access denied","Contact supervisor");
  addNotification("Unauthorized machine start detected!","alarm");

  
  char tgMsg[200];
  snprintf(tgMsg,200,
    "<b>⚠ UNAUTHORIZED ACCESS</b>\n"
    "Machine: started without valid card\n"
    "Time: %s\n"
    "Location: Machine Guardian",
    getDateString().c_str());
  sendTelegram(tgMsg);

  beepAlarm();
  startUnauthSession();  
  enterIdle();
}

void handleMachineOff() {
  if(currentState==STATE_RUNNING) {
    unsigned long sec=(millis()-machineStartTime)/1000;
    recordRuntime(currentEmployee,sec,false);
    char notif[128]; snprintf(notif,128,"Machine stopped. %s ran for %lus",currentEmployee.c_str(),sec);
    addNotification(notif,"info");
  }
  stopUnauthSession();
  enterIdle();
}

// ─── NFC ──────────────────────────────────────────────────────
void checkNFC() {
  uint8_t uid[7]={0}; uint8_t uidLen=0;
  if(!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A,uid,&uidLen,100)) return;
  if(uidLen==0||uidLen>7) return;
  String uidStr=uidToString(uid,uidLen);
  if(!uidStr.length()) return;

  String emp="";
  bool auth=isAuthorised(uidStr,emp);
  Serial.printf("NFC: %s → %s\n",uidStr.c_str(),auth?emp.c_str():"UNKNOWN");

  if(!auth) {
    char notif[128]; snprintf(notif,128,"Unknown card: %s",uidStr.c_str());
    addNotification(notif,"warn");
    displayMessage("Unknown Card",uidStr.c_str(),"Not authorised");
    beepShort(); delay(100); beepShort(); delay(1500);
    if(currentState==STATE_IDLE) enterIdle();
    else if(currentState==STATE_RUNNING){
      char l2[32]; snprintf(l2,32,"%.1fA / %.0fW",gFilteredNet,gFilteredNet*220.0f);
      displayMessage("Machine Running",l2,currentEmployee.c_str());
    }
    return;
  }

  if(currentState==STATE_IDLE||currentState==STATE_AUTHENTICATED){
    currentUID=uidStr; enterAuthenticated(emp);
  } else if(currentState==STATE_RUNNING){
    if(currentUID==uidStr){addNotification((emp+" checked out").c_str(),"info");beepShort();}
    else{
      char notif[128]; snprintf(notif,128,"%s scanned while %s is running",emp.c_str(),currentEmployee.c_str());
      addNotification(notif,"warn");
    }
  }
}

// ─── Dashboard HTML ───────────────────────────────────────────


// Dashboard HTML served from PROGMEM. All JS/CSS inline — no external files needed from ESP32.
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Machine Guardian</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/xlsx@0.18.5/dist/xlsx.full.min.js"></script>
<style>
@import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;700&family=Syne:wght@400;700;800&display=swap');
:root{--bg:#0a0c10;--surface:#111318;--border:#1e222b;--text:#e2e8f0;--muted:#64748b;
      --green:#22c55e;--yellow:#eab308;--red:#ef4444;--blue:#3b82f6;--accent:#6366f1;--orange:#f97316;}
*{box-sizing:border-box;margin:0;padding:0;}
body{background:var(--bg);color:var(--text);font-family:'Syne',sans-serif;min-height:100vh;}
header{border-bottom:1px solid var(--border);padding:20px 32px;display:flex;align-items:center;
       justify-content:space-between;position:sticky;top:0;background:var(--bg);z-index:10;}
.logo{font-size:20px;font-weight:800;letter-spacing:-.5px;} .logo span{color:var(--accent);}
nav{display:flex;gap:4px;}
.nav-btn{background:none;border:1px solid var(--border);color:var(--muted);padding:7px 16px;
         border-radius:8px;font-family:inherit;font-size:13px;font-weight:700;cursor:pointer;transition:all .15s;}
.nav-btn.active,.nav-btn:hover{background:var(--accent);border-color:var(--accent);color:#fff;}
#status-badge{display:flex;align-items:center;gap:8px;font-family:'JetBrains Mono',monospace;font-size:13px;color:var(--muted);}
#status-dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 6px var(--green);animation:pulse 2s infinite;}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
main{padding:32px;max-width:1400px;margin:0 auto;}
.page{display:none;} .page.active{display:block;}
.status-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:16px;margin-bottom:32px;}
.stat-card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:20px;position:relative;overflow:hidden;}
.stat-card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:var(--accent);}
.stat-card.green::before{background:var(--green);} .stat-card.yellow::before{background:var(--yellow);}
.stat-card.red::before{background:var(--red);}     .stat-card.blue::before{background:var(--blue);}
.stat-card.orange::before{background:var(--orange);}
.stat-label{font-size:11px;font-weight:700;letter-spacing:1.5px;text-transform:uppercase;color:var(--muted);margin-bottom:8px;}
.stat-value{font-size:24px;font-weight:800;line-height:1;font-family:'JetBrains Mono',monospace;}
.stat-value.green{color:var(--green);} .stat-value.yellow{color:var(--yellow);}
.stat-value.red{color:var(--red);}     .stat-value.blue{color:var(--blue);}
.stat-value.orange{color:var(--orange);}
.stat-sub{font-size:12px;color:var(--muted);margin-top:6px;}
/* live current gauge */
.current-gauge{margin-top:10px;}
.gauge-track{height:6px;border-radius:3px;background:var(--border);position:relative;overflow:visible;}
.gauge-fill{height:100%;border-radius:3px;background:var(--green);transition:width .4s;}
.gauge-marker{position:absolute;top:-4px;width:2px;height:14px;border-radius:1px;}
.gauge-marker.ambient{background:var(--yellow);} .gauge-marker.threshold{background:var(--red);}
.gauge-labels{display:flex;justify-content:space-between;font-size:10px;color:var(--muted);margin-top:4px;font-family:'JetBrains Mono',monospace;}
.charts-grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:32px;}
@media(max-width:900px){.charts-grid{grid-template-columns:1fr;}}
.chart-card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:24px;}
.chart-card.wide{grid-column:1/-1;}
.chart-title{font-size:13px;font-weight:700;letter-spacing:1px;text-transform:uppercase;
             color:var(--muted);margin-bottom:20px;display:flex;align-items:center;gap:8px;}
.chart-title-dot{width:6px;height:6px;border-radius:50%;}
canvas{max-height:260px;}
#machine-banner{border-radius:10px;padding:14px 20px;margin-bottom:28px;display:flex;
                align-items:center;gap:12px;font-weight:700;font-size:15px;border:1px solid;transition:all .4s;}
#machine-banner.idle         {background:rgba(239,68,68,.1); border-color:rgba(239,68,68,.3); color:var(--red);}
#machine-banner.authenticated{background:rgba(234,179,8,.1); border-color:rgba(234,179,8,.3); color:var(--yellow);}
#machine-banner.running      {background:rgba(34,197,94,.1); border-color:rgba(34,197,94,.3); color:var(--green);}
#machine-banner.unauthorized {background:rgba(239,68,68,.2); border-color:var(--red);         color:var(--red);}
.card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:24px;margin-bottom:20px;}
.card-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px;flex-wrap:wrap;gap:8px;}
.card-title{font-size:13px;font-weight:700;letter-spacing:1px;text-transform:uppercase;color:var(--muted);}
.badge{background:var(--accent);color:#fff;font-size:11px;font-weight:700;padding:2px 8px;border-radius:99px;font-family:'JetBrains Mono',monospace;}
.badge.red{background:var(--red);}
.notif-list{display:flex;flex-direction:column;gap:8px;}
.notif-item{display:flex;align-items:flex-start;gap:12px;padding:12px 14px;border-radius:8px;
            border:1px solid var(--border);font-size:13px;animation:slideIn .3s ease;}
@keyframes slideIn{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:translateY(0)}}
.notif-item.info {border-left:3px solid var(--green);}
.notif-item.warn {border-left:3px solid var(--yellow);}
.notif-item.alarm{border-left:3px solid var(--red);background:rgba(239,68,68,.05);}
.notif-time{font-family:'JetBrains Mono',monospace;font-size:11px;color:var(--muted);white-space:nowrap;margin-top:2px;}
.notif-msg{flex:1;line-height:1.5;}
.ndot{width:8px;height:8px;border-radius:50%;margin-top:4px;flex-shrink:0;}
.notif-item.info  .ndot{background:var(--green);}
.notif-item.warn  .ndot{background:var(--yellow);}
.notif-item.alarm .ndot{background:var(--red);box-shadow:0 0 6px var(--red);}
.emp-table{width:100%;border-collapse:collapse;font-size:14px;}
.emp-table th{text-align:left;padding:10px 14px;font-size:11px;font-weight:700;letter-spacing:1.2px;
              text-transform:uppercase;color:var(--muted);border-bottom:1px solid var(--border);}
.emp-table td{padding:12px 14px;border-bottom:1px solid var(--border);vertical-align:middle;}
.emp-table tr:last-child td{border-bottom:none;}
.emp-table tr:hover td{background:rgba(255,255,255,.02);}
.emp-name{font-weight:700;} .emp-unauth{color:var(--red);font-weight:700;}
.emp-status{display:inline-flex;align-items:center;gap:6px;font-size:12px;padding:3px 10px;border-radius:99px;}
.emp-status.active  {background:rgba(34,197,94,.12);color:var(--green);}
.emp-status.inactive{background:rgba(100,116,139,.12);color:var(--muted);}
.emp-status.unauth  {background:rgba(239,68,68,.12);color:var(--red);}
.icon-btn{background:none;border:1px solid var(--border);color:var(--muted);padding:5px 10px;
          border-radius:6px;cursor:pointer;font-size:12px;transition:all .15s;margin-left:4px;}
.icon-btn:hover{border-color:var(--red);color:var(--red);}
.icon-btn.toggle-btn:hover{border-color:var(--yellow);color:var(--yellow);}
.form-row{display:grid;grid-template-columns:1fr 1fr auto;gap:12px;align-items:end;margin-top:16px;}
@media(max-width:700px){.form-row{grid-template-columns:1fr;}}
.form-row-3{display:grid;grid-template-columns:1fr 1fr 1fr auto;gap:12px;align-items:end;margin-top:16px;}
@media(max-width:800px){.form-row-3{grid-template-columns:1fr 1fr;}}
.form-group{display:flex;flex-direction:column;gap:6px;}
.form-label{font-size:11px;font-weight:700;letter-spacing:1px;text-transform:uppercase;color:var(--muted);}
.form-input{background:var(--bg);border:1px solid var(--border);color:var(--text);padding:10px 14px;
            border-radius:8px;font-family:'JetBrains Mono',monospace;font-size:13px;outline:none;transition:border-color .15s;}
.form-input:focus{border-color:var(--accent);}
.btn{padding:10px 20px;border-radius:8px;font-family:inherit;font-size:13px;font-weight:700;
     cursor:pointer;border:none;letter-spacing:.5px;transition:opacity .15s;}
.btn:hover{opacity:.85;} .btn:disabled{opacity:.4;cursor:not-allowed;}
.btn-primary{background:var(--accent);color:#fff;}
.btn-success{background:var(--green);color:#000;}
.btn-danger {background:var(--red);color:#fff;}
.btn-excel  {background:#217346;color:#fff;}
.btn-warn   {background:var(--yellow);color:#000;}
.hint{font-size:12px;color:var(--muted);line-height:1.7;margin-top:8px;}
.info-grid{font-family:'JetBrains Mono',monospace;font-size:13px;line-height:2.2;color:var(--muted);}
.info-grid span{color:var(--text);}
.scan-hint{background:rgba(99,102,241,.1);border:1px solid rgba(99,102,241,.3);border-radius:8px;
           padding:12px 16px;font-size:13px;color:var(--accent);margin-top:12px;min-height:44px;}
.result-box{display:none;font-size:13px;color:var(--text);background:var(--bg);border:1px solid var(--border);
            border-radius:8px;padding:10px 14px;font-family:'JetBrains Mono',monospace;line-height:2;margin-top:12px;}
.amb-box{background:rgba(34,197,94,.08);border:1px solid rgba(34,197,94,.25);border-radius:10px;padding:20px;margin-bottom:16px;}
.amb-val{font-family:'JetBrains Mono',monospace;font-size:32px;font-weight:700;color:var(--green);margin:8px 0;}
.amb-label{font-size:11px;font-weight:700;letter-spacing:1.5px;text-transform:uppercase;color:var(--muted);}
.thr-box{background:rgba(239,68,68,.08);border:1px solid rgba(239,68,68,.25);border-radius:10px;padding:20px;margin-bottom:16px;}
.thr-val{font-family:'JetBrains Mono',monospace;font-size:32px;font-weight:700;color:var(--red);margin:8px 0;}
.dl-row{display:flex;gap:12px;flex-wrap:wrap;margin-top:16px;align-items:center;}
</style>
</head>
<body>
<header>
  <div class="logo">Machine<span>Guardian</span></div>
  <nav>
    <button class="nav-btn active" onclick="showPage('dashboard',this)">Dashboard</button>
    <button class="nav-btn"        onclick="showPage('employees',this)">Employees</button>
    <button class="nav-btn"        onclick="showPage('settings',this)">Settings</button>
  </nav>
  <div id="status-badge">
    <div id="status-dot"></div>
    <span id="status-text">Connecting...</span>
  </div>
</header>
<main>

<!-- DASHBOARD -->
<div id="page-dashboard" class="page active">
  <div id="machine-banner" class="idle">
    <span id="banner-icon">●</span>
    <span id="banner-text">Authentication Required</span>
  </div>
  <div class="status-row">
    <!-- Current card now always shows live raw amps with visual gauge -->
    <div class="stat-card green" style="grid-column:span 2">
      <div class="stat-label">Live Current</div>
      <div style="display:flex;align-items:baseline;gap:12px;flex-wrap:wrap">
        <div class="stat-value green" id="stat-raw">0.00A</div>
        <div style="font-size:13px;color:var(--muted)">raw &nbsp;|&nbsp; net: <span id="stat-net-inline" style="color:var(--green);font-family:'JetBrains Mono',monospace">0.00A</span></div>
      </div>
      <!-- Gauge: shows raw amps relative to ambient (yellow) and threshold+ambient (red) -->
      <div class="current-gauge" style="margin-top:10px">
        <div class="gauge-track" id="gauge-track">
          <div class="gauge-fill" id="gauge-fill" style="width:0%"></div>
          <div class="gauge-marker ambient"   id="gauge-ambient"   style="left:0%"></div>
          <div class="gauge-marker threshold" id="gauge-threshold" style="left:50%"></div>
        </div>
        <div class="gauge-labels">
          <span>0A</span>
          <span id="gauge-label-amb" style="color:var(--yellow)">Ambient</span>
          <span id="gauge-label-thr" style="color:var(--red)">Threshold</span>
          <span id="gauge-label-max">10A</span>
        </div>
      </div>
      <div class="stat-sub" id="stat-watts">0 W</div>
    </div>
    <div class="stat-card blue">
      <div class="stat-label">Session Runtime</div>
      <div class="stat-value blue" id="stat-runtime">00:00</div>
      <div class="stat-sub" id="stat-employee">No operator</div>
    </div>
    <div class="stat-card yellow">
      <div class="stat-label">Today's Runtime</div>
      <div class="stat-value yellow" id="stat-today">0h 0m</div>
      <div class="stat-sub">All authorised</div>
    </div>
    <div class="stat-card orange">
      <div class="stat-label">Unauth Runtime</div>
      <div class="stat-value orange" id="stat-unauth-time">0h 0m</div>
      <div class="stat-sub">Today, no valid card</div>
    </div>
    <div class="stat-card red">
      <div class="stat-label">Alerts Today</div>
      <div class="stat-value red" id="stat-alerts">0</div>
      <div class="stat-sub">Unauthorized events</div>
    </div>
  </div>
  <div class="charts-grid">
    <!-- Power chart shows ALL raw current samples + annotation lines for ambient and threshold -->
    <div class="chart-card wide">
      <div class="chart-title"><div class="chart-title-dot" style="background:var(--blue)"></div>
        Current Draw — all samples &nbsp;
        <span style="font-size:11px;color:var(--yellow)">── Ambient</span> &nbsp;
        <span style="font-size:11px;color:var(--red)">── Threshold</span>
      </div>
      <canvas id="powerChart"></canvas>
    </div>
    <div class="chart-card">
      <div class="chart-title"><div class="chart-title-dot" style="background:var(--green)"></div>Runtime by Day (minutes)</div>
      <canvas id="runtimeChart"></canvas>
    </div>
    <div class="chart-card">
      <div class="chart-title"><div class="chart-title-dot" style="background:var(--accent)"></div>Usage by Operator</div>
      <canvas id="usageChart"></canvas>
    </div>
  </div>
  <div class="card">
    <div class="card-header">
      <div class="card-title">Event Log</div>
      <div style="display:flex;gap:8px;align-items:center">
        <div class="badge" id="notif-count">0</div>
        <button class="btn btn-excel" style="padding:6px 14px;font-size:12px" onclick="downloadExcel()">Download Excel</button>
      </div>
    </div>
    <div class="notif-list" id="notif-list">
      <div style="color:var(--muted);font-size:13px;padding:12px 0">No events yet...</div>
    </div>
  </div>
</div>

<!-- EMPLOYEES -->
<div id="page-employees" class="page">
  <div class="card">
    <div class="card-header">
      <div class="card-title">Employee Roster</div>
      <div style="display:flex;gap:8px;align-items:center">
        <div class="badge" id="emp-count">0</div>
        <div class="badge red" id="unauth-badge" style="display:none">Unauth sessions today</div>
      </div>
    </div>
    <table class="emp-table">
      <thead><tr><th>Name</th><th>Status</th><th>Total Runtime</th><th>Actions</th></tr></thead>
      <tbody id="emp-tbody">
        <tr><td colspan="4" style="color:var(--muted);text-align:center;padding:24px">Loading...</td></tr>
      </tbody>
    </table>
  </div>
  <div class="card">
    <div class="card-header"><div class="card-title">Add New Employee</div></div>
    <p class="hint">Scan an unknown card — its UID appears in Event Log and auto-fills below.</p>
    <div class="scan-hint" id="uid-hint">Scan a card to see its UID here.</div>
    <div class="form-row">
      <div class="form-group">
        <label class="form-label">Employee Name</label>
        <input class="form-input" id="new-name" type="text" placeholder="Full name">
      </div>
      <div class="form-group">
        <label class="form-label">Card UID (hex)</label>
        <input class="form-input" id="new-uid" type="text" placeholder="e.g. A1B2C3D4" style="text-transform:uppercase">
      </div>
      <button class="btn btn-success" onclick="addEmployee()">Add &amp; Save</button>
    </div>
    <p class="hint" style="margin-top:12px">Saved permanently in device flash. Survives reboots and power cuts.</p>
  </div>
</div>

<!-- SETTINGS -->
<div id="page-settings" class="page">
  <!-- Threshold editor — most frequently changed setting -->
  <div class="card">
    <div class="card-header"><div class="card-title">Detection Threshold</div></div>
    <div class="thr-box">
      <div class="amb-label">Current threshold (net amps above ambient)</div>
      <div class="thr-val" id="thr-display">— A</div>
      <div style="font-size:12px;color:var(--muted)">Machine is detected ON when net current exceeds this value.</div>
    </div>
    <p class="hint">Set lower to detect small loads (phone charger ~0.3A net). Set higher to avoid false triggers from nearby equipment.</p>
    <div class="form-row" style="margin-top:12px">
      <div class="form-group">
        <label class="form-label">New threshold (A)</label>
        <input class="form-input" id="thr-input" type="number" step="0.1" min="0.1" max="50" placeholder="e.g. 1.5">
      </div>
      <div></div>
      <button class="btn btn-warn" onclick="setThreshold()">Apply &amp; Save</button>
    </div>
    <div id="thr-result" class="result-box"></div>
  </div>

  <!-- Ambient calibration -->
  <div class="card">
    <div class="card-header"><div class="card-title">Ambient Calibration</div></div>
    <div class="amb-box">
      <div class="amb-label">Current ambient baseline</div>
      <div class="amb-val" id="amb-val">— A</div>
      <div style="font-size:12px;color:var(--muted)">Net = Raw − Ambient. All chart lines reference this.</div>
    </div>
    <p class="hint">Machine must be <b style="color:var(--red)">completely OFF</b> before capturing. Takes ~3 seconds.</p>
    <div style="margin-top:16px;display:flex;gap:12px;flex-wrap:wrap">
      <button class="btn btn-primary" id="amb-btn" onclick="captureAmbient()">Capture Ambient</button>
      <div id="amb-result" class="result-box"></div>
    </div>
  </div>

  <!-- Excel download -->
  <div class="card">
    <div class="card-header"><div class="card-title">Download Data</div></div>
    <p class="hint">Full Excel report: Power Log, Runtime by Day, Operator Summary, Event Log in separate sheets.</p>
    <div class="dl-row">
      <button class="btn btn-excel" onclick="downloadExcel()">Download Excel Report</button>
      <span style="font-size:12px;color:var(--muted)" id="dl-status"></span>
    </div>
  </div>

  <!-- Device info -->
  <div class="card">
    <div class="card-header"><div class="card-title">Device Info</div></div>
    <div class="info-grid">
      <div>ADC midpoint &nbsp;&nbsp;: <span id="info-mid">—</span></div>
      <div>Raw RMS &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;: <span id="info-raw">—</span> A</div>
      <div>Ambient &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;: <span id="info-amb">—</span> A</div>
      <div>Net current &nbsp;&nbsp;&nbsp;: <span id="info-net">—</span> A</div>
      <div>Threshold &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;: <span id="info-thr">—</span> A net</div>
      <div>Device IP &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;: <span id="info-ip" style="color:var(--accent)">—</span></div>
    </div>
  </div>

  <!-- Danger zone -->
  <div class="card">
    <div class="card-header"><div class="card-title">Data Management</div></div>
    <p class="hint">Clear all employees permanently. Cannot be undone.</p>
    <div style="margin-top:16px">
      <button class="btn btn-danger" onclick="clearEmployees()">Clear All Employees</button>
    </div>
  </div>
</div>

</main>
<script>
const API=window.location.origin;
let lastNotifCount=0,alertsToday=0,empRuntimeMap={};
// realPowerData: samples from the device (ts >= bootMark).
// demoPowerData: synthetic past-3-day samples, displayed left of the "now" divider.
// allRuntimeData: merged demo+real runtime entries.
// allNotifData: notification log from device.
let realPowerData=[],demoPowerData=[],allRuntimeData=[],allNotifData=[];
// demoRuntimeData kept separately so Excel can exclude it.
let demoRuntimeData=[];
// nvsEmployeeNames: fetched once from /api/employees, used to seed demo data with real names.
let nvsEmployeeNames=[];
// bootMark: unix timestamp of page load — everything before this is demo.
const bootMark=Math.floor(Date.now()/1000);
// gAmbient, gThreshold, gRawAmps: live values updated every 2s from /api/status.
let gAmbient=0,gThreshold=1,gRawAmps=0;
// nowDividerIdx: index in the merged chart labels where the "NOW" vertical line sits.
let nowDividerIdx=-1;

Chart.defaults.color='#64748b';
Chart.defaults.borderColor='#1e222b';
Chart.defaults.font.family="'JetBrains Mono',monospace";
Chart.defaults.font.size=11;

// Power chart — 5 datasets: raw amps, watts/100, ambient line, threshold line, divider.
// The divider dataset is a single null-filled array with one spike at nowDividerIdx.
const powerChart=new Chart(document.getElementById('powerChart'),{
  type:'line',
  data:{labels:[],datasets:[
    {label:'Raw Amps',data:[],borderColor:'#3b82f6',backgroundColor:'rgba(59,130,246,.08)',
     borderWidth:2,pointRadius:0,fill:true,tension:0.4,order:1},
    {label:'Watts /100',data:[],borderColor:'#6366f1',backgroundColor:'rgba(99,102,241,.04)',
     borderWidth:1,pointRadius:0,fill:false,tension:0.4,yAxisID:'y2',order:2},
    {label:'Ambient',data:[],borderColor:'#eab308',borderWidth:1.5,borderDash:[6,4],
     pointRadius:0,fill:false,tension:0,order:0},
    {label:'Threshold',data:[],borderColor:'#ef4444',borderWidth:1.5,borderDash:[6,4],
     pointRadius:0,fill:false,tension:0,order:0},
    {label:'Now',data:[],borderColor:'rgba(255,255,255,0.35)',borderWidth:2,borderDash:[3,3],
     pointRadius:0,fill:false,tension:0,order:0,yAxisID:'divider'}
  ]},
  options:{responsive:true,maintainAspectRatio:true,interaction:{mode:'index',intersect:false},
    plugins:{legend:{display:true,labels:{boxWidth:20,filter:i=>i.text!=='Now'}}},
    scales:{
      x:{grid:{color:'#1e222b'}},
      y:{grid:{color:'#1e222b'},title:{display:true,text:'A'},min:0},
      y2:{position:'right',grid:{display:false},title:{display:true,text:'W'}},
      divider:{display:false,min:0,max:1}
    }}
});

const runtimeChart=new Chart(document.getElementById('runtimeChart'),{
  type:'bar',
  data:{labels:[],datasets:[
    {label:'Authorised',  data:[],backgroundColor:'rgba(34,197,94,.7)', borderColor:'#22c55e',borderWidth:1,borderRadius:4},
    {label:'Unauthorised',data:[],backgroundColor:'rgba(239,68,68,.7)', borderColor:'#ef4444',borderWidth:1,borderRadius:4}
  ]},
  options:{responsive:true,maintainAspectRatio:true,plugins:{legend:{display:true}},
    scales:{x:{stacked:true,grid:{color:'#1e222b'}},y:{stacked:true,grid:{color:'#1e222b'},title:{display:true,text:'Min'}}}}
});

const usageChart=new Chart(document.getElementById('usageChart'),{
  type:'doughnut',
  data:{labels:[],datasets:[{data:[],
  backgroundColor:['#6366f1','#22c55e','#eab308','#3b82f6','#f97316','#ef4444','#ec4899'],
  borderColor:'#111318',borderWidth:2,hoverOffset:6}]},
  options:{responsive:true,maintainAspectRatio:true,plugins:{legend:{position:'right'}}}
});

// Builds demo power samples for past 3 days using real employee names where available.
// Each day has a morning session (08-12) and afternoon session (13-17) at realistic amps.
function buildDemoData(empNames) {
  const names = empNames && empNames.length>0
    ? empNames
    : ['Operator A','Operator B','Operator C'];
  const now=Math.floor(Date.now()/1000);
  const demoP=[], demoR=[];
  for(let d=3;d>=1;d--){
    const base=now-d*86400;
    for(let m=0;m<48;m++) demoP.push({ts:base+8*3600+m*300,amps:3.5+Math.random()*0.6,watts:(3.5+Math.random()*0.6)*220,demo:true});
    for(let m=0;m<48;m++) demoP.push({ts:base+13*3600+m*300,amps:4.0+Math.random()*0.5,watts:(4.0+Math.random()*0.5)*220,demo:true});
    const date=new Date(base*1000);
    const ds=`${date.getFullYear()}-${String(date.getMonth()+1).padStart(2,'0')}-${String(date.getDate()).padStart(2,'0')}`;
    demoR.push({date:ds,employee:names[d%names.length],       seconds:4*3600,unauthorized:false,demo:true});
    demoR.push({date:ds,employee:names[(d+1)%names.length],   seconds:3*3600,unauthorized:false,demo:true});
    if(d===2) demoR.push({date:ds,employee:'UNAUTHORIZED',seconds:1200,unauthorized:true,demo:true});
  }
  return {power:demoP,runtime:demoR};
}

function showPage(name,btn){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.nav-btn').forEach(b=>b.classList.remove('active'));
  document.getElementById('page-'+name).classList.add('active');
  btn.classList.add('active');
  if(name==='employees'){fetchEmployees();fetchRuntime();}
  if(name==='settings')fetchStatus();
}

function fmtTime(s){
  const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;
  if(h>0)return h+'h '+m+'m'; return String(m).padStart(2,'0')+':'+String(ss).padStart(2,'0');
}
function fmtTs(ts){return new Date(ts*1000).toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit',second:'2-digit'});}
function dayOf(ts){const d=new Date(ts*1000);return `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,'0')}-${String(d.getDate()).padStart(2,'0')}`;}

function updateBanner(state,emp,amps){
  const b=document.getElementById('machine-banner');
  const map=[
    ['idle',          '●','Authentication Required'],
    ['authenticated', '◐',`Machine Ready — ${emp}`],
    ['running',       '◉',`Machine Running — ${emp} — ${parseFloat(amps).toFixed(1)}A net`],
    ['unauthorized',  '⚠','⚠ UNAUTHORIZED ACCESS DETECTED']
  ];
  const[cls,icon,text]=map[Math.min(state,3)];
  b.className=cls;
  document.getElementById('banner-icon').textContent=icon;
  document.getElementById('banner-text').textContent=text;
}

function updateGauge(rawAmps,ambient,threshold){
  const gaugeMax=Math.max((ambient+threshold)*2,10);
  const pct=v=>Math.min(100,Math.max(0,(v/gaugeMax)*100));
  document.getElementById('gauge-fill').style.width=pct(rawAmps)+'%';
  document.getElementById('gauge-fill').style.background=
    rawAmps>(ambient+threshold)?'var(--green)': rawAmps>ambient?'var(--yellow)':'var(--muted)';
  document.getElementById('gauge-ambient').style.left=pct(ambient)+'%';
  document.getElementById('gauge-threshold').style.left=pct(ambient+threshold)+'%';
  document.getElementById('gauge-label-amb').textContent=ambient.toFixed(1)+'A ambient';
  document.getElementById('gauge-label-thr').textContent=(ambient+threshold).toFixed(1)+'A thresh';
  document.getElementById('gauge-label-max').textContent=gaugeMax.toFixed(0)+'A';
}

async function fetchStatus(){
  try{
    const d=await fetch(`${API}/api/status`).then(r=>r.json());
    document.getElementById('status-text').textContent='Live';
    document.getElementById('status-dot').style.background='var(--green)';
    gAmbient=parseFloat(d.ambient_amps)||0;
    gThreshold=parseFloat(d.threshold)||1;
    gRawAmps=parseFloat(d.raw_amps)||0;
    const net=parseFloat(d.net_amps)||0;
    document.getElementById('stat-raw').textContent=gRawAmps.toFixed(2)+'A';
    document.getElementById('stat-net-inline').textContent=net.toFixed(2)+'A';
    const watts=d.state===2?net*220:0;
    document.getElementById('stat-watts').textContent=watts.toFixed(0)+' W  (net)';
    document.getElementById('stat-runtime').textContent=fmtTime(d.session_runtime_sec);
    document.getElementById('stat-employee').textContent=d.employee||'No operator';
    document.getElementById('info-mid').textContent=parseFloat(d.midpoint).toFixed(1);
    document.getElementById('info-raw').textContent=gRawAmps.toFixed(3);
    document.getElementById('info-amb').textContent=gAmbient.toFixed(3);
    document.getElementById('info-net').textContent=net.toFixed(3);
    document.getElementById('info-thr').textContent=gThreshold.toFixed(2);
    document.getElementById('amb-val').textContent=gAmbient.toFixed(3)+' A';
    document.getElementById('thr-display').textContent=gThreshold.toFixed(2)+' A';
    if(document.getElementById('info-ip').textContent==='—')
      document.getElementById('info-ip').textContent=window.location.hostname;
    updateBanner(d.state,d.employee,net);
    updateGauge(gRawAmps,gAmbient,gThreshold);
    // Also push a live data point to the chart every status poll so it updates in real-time.
    // The power chart polls every 10s from /api/power, but we push a live point every 2s here.
    pushLivePoint(gRawAmps);
  }catch{
    document.getElementById('status-text').textContent='Offline';
    document.getElementById('status-dot').style.background='var(--red)';
  }
}

// Pushes a single real-time point to realPowerData and triggers a chart redraw.
// This runs every 2s (from fetchStatus) so the chart updates smoothly between 10s power polls.
function pushLivePoint(rawAmps){
  const now=Math.floor(Date.now()/1000);
  realPowerData.push({ts:now, amps:rawAmps, watts:rawAmps*220, demo:false});
  // Cap to 288 real points (24h at 5s sampling cadence is ~17280 but 288 keeps memory sane)
  if(realPowerData.length>288) realPowerData.shift();
  rebuildPowerChart();
}

// Merges demoPowerData + realPowerData into one sorted array, finds the divider index,
// then updates all 5 chart datasets. Called whenever data or ambient/threshold changes.
function rebuildPowerChart(){
  // Sorted merge: demo points (past) come first, real points (present) after
  const merged=[...demoPowerData,...realPowerData].sort((a,b)=>a.ts-b.ts);
  if(merged.length===0) return;

  // Find the index of the first real (non-demo) point — that's where "NOW" divider goes
  nowDividerIdx=merged.findIndex(p=>!p.demo);
  if(nowDividerIdx<0) nowDividerIdx=merged.length; // all demo, divider at end

  const labels=merged.map(p=>{
    const d=new Date(p.ts*1000);
    // Show date + time for the divider vicinity, time only elsewhere
    return d.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit'});
  });
  // Override the divider label so the tooltip says "NOW"
  if(nowDividerIdx<labels.length) labels[nowDividerIdx]='▶ NOW';

  const n=merged.length;
  // Divider dataset: null everywhere except at nowDividerIdx where it spans 0→1 on hidden y axis
  const divData=Array(n).fill(null);
  if(nowDividerIdx<n){ divData[nowDividerIdx]=0; }
  // We need two points to draw a vertical-ish line — set the point to a high value
  // Actually Chart.js line charts need a workaround: we use a bar overlay trick via two points
  // Simpler: set pointRadius=4 at divider index only, and use NaN elsewhere for line gap.
  // Best approach for a vertical "now" line: use a plugin or annotation — but we keep it
  // dependency-free by inserting a marker label and making the divider point visually distinct.
  const ampsData=merged.map(p=>p.amps);
  const wattsData=merged.map(p=>p.watts/100);

  powerChart.data.labels=labels;
  powerChart.data.datasets[0].data=ampsData;
  powerChart.data.datasets[0].pointRadius=merged.map((_,i)=>i===nowDividerIdx?5:0);
  powerChart.data.datasets[0].pointBackgroundColor=merged.map((_,i)=>i===nowDividerIdx?'#fff':'#3b82f6');
  powerChart.data.datasets[1].data=wattsData;
  powerChart.data.datasets[2].data=Array(n).fill(gAmbient);
  powerChart.data.datasets[3].data=Array(n).fill(gAmbient+gThreshold);
  // Divider "line": spike at nowDividerIdx — null on all other points so Chart.js skips them
  const divSpike=Array(n).fill(null);
  if(nowDividerIdx>0 && nowDividerIdx<n){
    // Use the y-axis max as the spike height so it fills the chart vertically
    divSpike[nowDividerIdx-1]=0;
    divSpike[nowDividerIdx]=1;
  }
  powerChart.data.datasets[4].data=divSpike;
  powerChart.update('none');
}

// Fetches real power samples from the device and merges with demo data.
// On first load (empty realPowerData), builds demo data using NVS employee names.
async function fetchPower(){
  const d=await fetch(`${API}/api/power`).then(r=>r.json());
  if(d.length>0){
    // Tag all server samples as real (non-demo)
    realPowerData=d.map(p=>({...p,demo:false}));
  }
  // Build demo data once if we haven't yet
  if(demoPowerData.length===0){
    const demo=buildDemoData(nvsEmployeeNames);
    demoPowerData=demo.power;
    if(demoRuntimeData.length===0){
      demoRuntimeData=demo.runtime;
      // Seed allRuntimeData with demo if device has no real runtime yet
      const rt=await fetch(`${API}/api/runtime`).then(r=>r.json());
      allRuntimeData=[...demoRuntimeData,...rt.map(e=>({...e,demo:false}))];
    }
  }
  rebuildPowerChart();
}

// Fetches runtime from device, merges with demo runtime (demo kept for column chart only).
async function fetchRuntime(){
  const d=await fetch(`${API}/api/runtime`).then(r=>r.json());
  // Real data from device is always non-demo
  const realRuntime=d.map(e=>({...e,demo:false}));
  allRuntimeData=[...demoRuntimeData,...realRuntime];

  const dayAuth={},dayUnauth={},empMap={};
  let todayAuth=0,todayUnauth=0;
  const today=new Date().toISOString().split('T')[0];
  allRuntimeData.forEach(e=>{
    if(e.unauthorized){
      dayUnauth[e.date]=(dayUnauth[e.date]||0)+e.seconds/60;
      empMap['UNAUTHORIZED']=(empMap['UNAUTHORIZED']||0)+e.seconds;
      if(e.date===today)todayUnauth+=e.seconds;
    } else {
      dayAuth[e.date]=(dayAuth[e.date]||0)+e.seconds/60;
      empMap[e.employee]=(empMap[e.employee]||0)+e.seconds;
      if(e.date===today)todayAuth+=e.seconds;
    }
  });
  empRuntimeMap=empMap;
  const allDays=[...new Set([...Object.keys(dayAuth),...Object.keys(dayUnauth)])].sort();
  runtimeChart.data.labels=allDays;
  runtimeChart.data.datasets[0].data=allDays.map(d=>Math.round(dayAuth[d]||0));
  runtimeChart.data.datasets[1].data=allDays.map(d=>Math.round(dayUnauth[d]||0));
  runtimeChart.update('none');
  // Usage pie: only real data (no demo employees in pie)
  const realEmpMap={};
  realRuntime.forEach(e=>{
    const k=e.unauthorized?'UNAUTHORIZED':e.employee;
    realEmpMap[k]=(realEmpMap[k]||0)+e.seconds;
  });
  usageChart.data.labels=Object.keys(realEmpMap);
  usageChart.data.datasets[0].data=Object.values(realEmpMap).map(v=>Math.round(v/60));
  usageChart.update('none');
  document.getElementById('stat-today').textContent=fmtTime(todayAuth);
  document.getElementById('stat-unauth-time').textContent=fmtTime(todayUnauth);
  if(todayUnauth>0) document.getElementById('unauth-badge').style.display='inline-flex';
}

async function fetchNotifications(){
  const d=await fetch(`${API}/api/notifications`).then(r=>r.json());
  allNotifData=d;
  document.getElementById('notif-count').textContent=d.length;
  if(d.length===lastNotifCount)return;
  lastNotifCount=d.length;
  alertsToday=d.filter(n=>n.type==='alarm').length;
  document.getElementById('stat-alerts').textContent=alertsToday;
  const unk=[...d].reverse().find(n=>n.message.startsWith('Unknown card:'));
  if(unk){
    const uid=unk.message.replace('Unknown card: ','').trim();
    document.getElementById('uid-hint').innerHTML=
      `Last unknown card: <b style="color:var(--yellow);font-family:'JetBrains Mono',monospace">${uid}</b>
       &nbsp;<button class="btn btn-success" style="padding:4px 12px;font-size:12px"
         onclick="document.getElementById('new-uid').value='${uid}';showPage('employees',document.querySelectorAll('.nav-btn')[1])">
         Use this UID</button>`;
  }
  const list=document.getElementById('notif-list');
  if(!d.length){list.innerHTML='<div style="color:var(--muted);font-size:13px;padding:12px 0">No events yet...</div>';return;}
  list.innerHTML=d.slice().reverse().map(n=>`
    <div class="notif-item ${n.type}">
      <div class="ndot"></div>
      <div class="notif-msg">${n.message}</div>
      <div class="notif-time">${fmtTs(n.timestamp)}</div>
    </div>`).join('');
}

// Fetches employee list from device. Also seeds nvsEmployeeNames for demo data generation.
async function fetchEmployees(){
  const d=await fetch(`${API}/api/employees`).then(r=>r.json());
  nvsEmployeeNames=d.map(e=>e.name).filter(Boolean);
  document.getElementById('emp-count').textContent=d.length;
  const tb=document.getElementById('emp-tbody');
  if(!d.length){tb.innerHTML='<tr><td colspan="4" style="color:var(--muted);text-align:center;padding:24px">No employees registered</td></tr>';return;}
  const unauthSec=empRuntimeMap['UNAUTHORIZED']||0;
  const rows=d.map((e,i)=>`<tr>
    <td><div class="emp-name">${e.name}</div></td>
    <td><span class="emp-status ${e.active?'active':'inactive'}">${e.active?'● Active':'○ Inactive'}</span></td>
    <td style="font-family:'JetBrains Mono',monospace;font-size:13px;color:var(--muted)">${fmtTime(empRuntimeMap[e.name]||0)}</td>
    <td>
      <button class="icon-btn toggle-btn" onclick="toggleEmployee(${i})">${e.active?'Deactivate':'Activate'}</button>
      <button class="icon-btn"            onclick="removeEmployee(${i})">Remove</button>
    </td></tr>`);
  if(unauthSec>0) rows.push(`<tr style="background:rgba(239,68,68,.04)">
    <td><div class="emp-unauth">⚠ UNAUTHORIZED</div></td>
    <td><span class="emp-status unauth">● Active today</span></td>
    <td style="font-family:'JetBrains Mono',monospace;font-size:13px;color:var(--red)">${fmtTime(unauthSec)}</td>
    <td><span style="font-size:12px;color:var(--muted)">No card presented</span></td></tr>`);
  tb.innerHTML=rows.join('');
}

async function addEmployee(){
  const name=document.getElementById('new-name').value.trim();
  const uid=document.getElementById('new-uid').value.trim().toUpperCase();
  if(!name||!uid){alert('Enter both name and UID');return;}
  if(!/^[0-9A-F]+$/.test(uid)){alert('UID must be hex only (0-9, A-F)');return;}
  const r=await fetch(`${API}/api/employees/add`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,uid})});
  if(!r.ok){alert('Failed to add employee');return;}
  document.getElementById('new-name').value=''; document.getElementById('new-uid').value='';
  fetchEmployees();
}

async function removeEmployee(idx){
  if(!confirm('Remove this employee?'))return;
  await fetch(`${API}/api/employees/remove`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:idx})});
  fetchEmployees();
}

async function toggleEmployee(idx){
  await fetch(`${API}/api/employees/toggle`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:idx})});
  fetchEmployees();
}

async function clearEmployees(){
  if(!confirm('Delete ALL employees permanently?'))return;
  await fetch(`${API}/api/employees/clear`,{method:'POST'}); fetchEmployees();
}

async function captureAmbient(){
  const btn=document.getElementById('amb-btn');
  const res=document.getElementById('amb-result');
  btn.textContent='Capturing (~3s)...'; btn.disabled=true; res.style.display='none';
  try{
    const d=await fetch(`${API}/api/set_ambient`).then(r=>r.json());
    res.innerHTML=`Raw avg: <b>${parseFloat(d.raw_avg).toFixed(3)} A</b><br>`+
      `Saved: <b style="color:var(--green)">${parseFloat(d.ambient_amps).toFixed(3)} A</b> (incl. 10% margin)`;
    res.style.display='block';
    gAmbient=parseFloat(d.ambient_amps);
    document.getElementById('amb-val').textContent=gAmbient.toFixed(3)+' A';
    rebuildPowerChart();
  }catch{res.textContent='Failed';res.style.display='block';}
  btn.textContent='Capture Ambient'; btn.disabled=false;
}

async function setThreshold(){
  const val=parseFloat(document.getElementById('thr-input').value);
  if(isNaN(val)||val<0.1||val>50){alert('Enter a value between 0.1 and 50');return;}
  const res=document.getElementById('thr-result');
  res.style.display='none';
  const r=await fetch(`${API}/api/set_threshold`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({threshold:val})});
  if(!r.ok){res.textContent='Failed to set threshold';res.style.display='block';return;}
  gThreshold=val;
  document.getElementById('thr-display').textContent=val.toFixed(2)+' A';
  res.innerHTML=`<span style="color:var(--green)">Threshold set to ${val.toFixed(2)} A and saved.</span>`;
  res.style.display='block';
  rebuildPowerChart();
}

// Excel export — demo data (demo:true) is excluded from all sheets. Only real device data is written.
async function downloadExcel(){
  const status=document.getElementById('dl-status');
  if(status)status.textContent='Building file...';
  const wb=XLSX.utils.book_new();
  // Power log — real samples only
  const realPwr=realPowerData.filter(p=>!p.demo);
  const pr=[['Timestamp','Date','Time','Raw Amps','Net Amps','Watts']];
  realPwr.forEach(p=>{
    const d=new Date(p.ts*1000);
    const net=Math.max(0,p.amps-gAmbient);
    pr.push([d.toLocaleString('en-GB'),d.toLocaleDateString('en-GB'),
      d.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit'}),
      parseFloat(p.amps.toFixed(3)),parseFloat(net.toFixed(3)),parseFloat(p.watts.toFixed(1))]);
  });
  const ws1=XLSX.utils.aoa_to_sheet(pr);
  ws1['!cols']=[{wch:20},{wch:12},{wch:10},{wch:12},{wch:12},{wch:12}];
  XLSX.utils.book_append_sheet(wb,ws1,'Power Log');
  // Runtime — real entries only
  const realRt=allRuntimeData.filter(e=>!e.demo);
  const rr=[['Date','Operator','Runtime (min)','Runtime (hh:mm)','Type']];
  realRt.forEach(e=>{
    const h=Math.floor(e.seconds/3600),m=Math.floor((e.seconds%3600)/60);
    rr.push([e.date,e.employee,Math.round(e.seconds/60),`${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}`,e.unauthorized?'UNAUTHORIZED':'Authorised']);
  });
  const ws2=XLSX.utils.aoa_to_sheet(rr);
  ws2['!cols']=[{wch:12},{wch:24},{wch:14},{wch:14},{wch:14}];
  XLSX.utils.book_append_sheet(wb,ws2,'Runtime by Day');
  // Operator summary — real only
  const empAgg={};
  realRt.forEach(e=>{
    const k=e.employee+(e.unauthorized?'_U':'_A');
    if(!empAgg[k])empAgg[k]={name:e.employee,seconds:0,unauth:e.unauthorized};
    empAgg[k].seconds+=e.seconds;
  });
  const sr=[['Operator','Total Runtime (min)','Total Runtime (hh:mm)','Type']];
  Object.values(empAgg).forEach(e=>{
    const h=Math.floor(e.seconds/3600),m=Math.floor((e.seconds%3600)/60);
    sr.push([e.name,Math.round(e.seconds/60),`${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}`,e.unauth?'UNAUTHORIZED':'Authorised']);
  });
  const ws3=XLSX.utils.aoa_to_sheet(sr);
  ws3['!cols']=[{wch:24},{wch:20},{wch:18},{wch:14}];
  XLSX.utils.book_append_sheet(wb,ws3,'Operator Summary');
  // Event log
  const er=[['Timestamp','Date','Time','Message','Type']];
  allNotifData.forEach(n=>{
    const d=new Date(n.timestamp*1000);
    er.push([d.toLocaleString('en-GB'),d.toLocaleDateString('en-GB'),
      d.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit',second:'2-digit'}),n.message,n.type.toUpperCase()]);
  });
  const ws4=XLSX.utils.aoa_to_sheet(er);
  ws4['!cols']=[{wch:20},{wch:12},{wch:10},{wch:60},{wch:10}];
  XLSX.utils.book_append_sheet(wb,ws4,'Event Log');
  const now=new Date();
  const fname=`MachineGuardian_${now.getFullYear()}-${String(now.getMonth()+1).padStart(2,'0')}-${String(now.getDate()).padStart(2,'0')}.xlsx`;
  XLSX.writeFile(wb,fname);
  if(status){status.textContent='Downloaded: '+fname; setTimeout(()=>status.textContent='',4000);}
}

// Boot sequence: fetch employees first so demo data can use real names, then fetch everything else.
(async()=>{
  await fetchEmployees();
  await fetchPower();
  fetchRuntime();
  fetchNotifications();
  fetchStatus();
})();

setInterval(fetchStatus,        2000);
setInterval(fetchPower,        30000);
setInterval(fetchRuntime,      15000);
setInterval(fetchNotifications, 3000);
</script>
</body>
</html>
)rawhtml";


// ─── Web handlers ─────────────────────────────────────────────

void handleRoot() { server.send_P(200,"text/html",DASHBOARD_HTML); }

// Returns current sensor state, filtered and raw amps, ambient, threshold — used by dashboard every 2s.
void handleStatus() {
  float raw=measureRawRMS();
  StaticJsonDocument<320> doc;
  doc["state"]               = (int)currentState;
  doc["machine_on"]          = machineOn;
  doc["employee"]            = currentEmployee;
  doc["raw_amps"]            = raw;
  doc["ambient_amps"]        = gAmbientAmps;
  doc["net_amps"]            = gFilteredNet;
  doc["midpoint"]            = gMidpoint;
  doc["threshold"]           = gThreshold;
  doc["session_runtime_sec"] = (currentState==STATE_RUNNING)?(millis()-machineStartTime)/1000:0;
  String out; serializeJson(doc,out);
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200,"application/json",out);
}

void handlePower() {
  String j="[";
  int start=(powerLogCount<MAX_SAMPLES)?0:powerLogHead%MAX_SAMPLES;
  for(int i=0;i<min(powerLogCount,MAX_SAMPLES);i++){
    int idx=(start+i)%MAX_SAMPLES; if(i)j+=",";
    j+="{\"ts\":"+String(powerLog[idx].ts)+",\"amps\":"+String(powerLog[idx].amps,2)+",\"watts\":"+String(powerLog[idx].watts,0)+"}";
  }
  j+="]"; server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json",j);
}

void handleRuntime() {
  String j="[";
  for(int i=0;i<runtimeLogCount;i++){
    if(i)j+=",";
    String emp=String(runtimeLog[i].employee); emp.replace("\"","'");
    j+="{\"date\":\""+String(runtimeLog[i].date)+"\""+
       ",\"employee\":\""+emp+"\""+
       ",\"seconds\":"+String(runtimeLog[i].seconds)+
       ",\"unauthorized\":"+(runtimeLog[i].unauthorized?"true":"false")+"}";
  }
  j+="]"; server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json",j);
}

void handleNotifications() {
  String j="["; bool first=true;
  int start=(notifCount<50)?0:notifHead%50;
  for(int i=0;i<min(notifCount,50);i++){
    int idx=(start+i)%50; if(!first)j+=","; first=false;
    String msg=String(notifLog[idx].message); msg.replace("\"","'");
    j+="{\"timestamp\":"+String(notifLog[idx].ts)+
       ",\"message\":\""+msg+"\""+
       ",\"type\":\""+String(notifLog[idx].type)+"\"}";
  }
  j+="]"; server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json",j);
}

void handleGetEmployees() {
  String j="[";
  for(int i=0;i<employeeCount;i++){
    if(i)j+=",";
    j+="{\"name\":\""+String(employees[i].name)+"\",\"active\":"+(employees[i].active?"true":"false")+"}";
  }
  j+="]"; server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json",j);
}

void handleAddEmployee() {
  if(!server.hasArg("plain")){server.send(400,"text/plain","no body");return;}
  StaticJsonDocument<128> doc;
  if(deserializeJson(doc,server.arg("plain"))){server.send(400,"text/plain","bad json");return;}
  if(employeeCount>=MAX_EMPLOYEES){server.send(507,"text/plain","full");return;}
  String name=doc["name"].as<String>(); String uid=doc["uid"].as<String>(); uid.toUpperCase();
  if(!name.length()||!uid.length()){server.send(400,"text/plain","missing");return;}
  strncpy(employees[employeeCount].name,name.c_str(),63);
  strncpy(employees[employeeCount].uid, uid.c_str(), 19);
  employees[employeeCount].active=true; employeeCount++;
  nvsSaveEmployees();
  char notif[128]; snprintf(notif,128,"Employee added: %s",name.c_str());
  addNotification(notif,"info");
  server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json","{\"ok\":true}");
}

void handleRemoveEmployee() {
  if(!server.hasArg("plain")){server.send(400,"text/plain","no body");return;}
  StaticJsonDocument<64> doc;
  if(deserializeJson(doc,server.arg("plain"))){server.send(400,"text/plain","bad json");return;}
  int idx=doc["index"].as<int>();
  if(idx<0||idx>=employeeCount){server.send(400,"text/plain","bad index");return;}
  char notif[128]; snprintf(notif,128,"Employee removed: %s",employees[idx].name);
  for(int i=idx;i<employeeCount-1;i++) employees[i]=employees[i+1];
  employeeCount--; nvsSaveEmployees();
  addNotification(notif,"info");
  server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json","{\"ok\":true}");
}

void handleToggleEmployee() {
  if(!server.hasArg("plain")){server.send(400,"text/plain","no body");return;}
  StaticJsonDocument<64> doc;
  if(deserializeJson(doc,server.arg("plain"))){server.send(400,"text/plain","bad json");return;}
  int idx=doc["index"].as<int>();
  if(idx<0||idx>=employeeCount){server.send(400,"text/plain","bad index");return;}
  employees[idx].active=!employees[idx].active; nvsSaveEmployees();
  server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json","{\"ok\":true}");
}

void handleClearEmployees() {
  employeeCount=0; nvsSaveEmployees();
  addNotification("All employees cleared","warn");
  server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json","{\"ok\":true}");
}

// Captures ambient baseline: 30 samples averaged + 10% margin, saved to NVS.
void handleSetAmbient() {
  float rawAvg=captureAmbient();
  StaticJsonDocument<128> doc;
  doc["raw_avg"]=rawAvg/1.1f; doc["ambient_amps"]=gAmbientAmps; doc["threshold"]=gThreshold;
  char notif[80]; snprintf(notif,80,"Ambient baseline set to %.3fA",gAmbientAmps);
  addNotification(notif,"info");
  String out; serializeJson(doc,out);
  server.sendHeader("Access-Control-Allow-Origin","*"); server.send(200,"application/json",out);
}

// Updates gThreshold from POST body {"threshold": X}. Persists to NVS immediately.
void handleSetThreshold() {
  if(!server.hasArg("plain")){server.send(400,"text/plain","no body");return;}
  StaticJsonDocument<64> doc;
  if(deserializeJson(doc,server.arg("plain"))){server.send(400,"text/plain","bad json");return;}
  float val=doc["threshold"].as<float>();
  if(val<0.1f||val>50.0f){server.send(400,"text/plain","out of range 0.1-50");return;}
  gThreshold=val;
  nvsSaveCalibration();
  char notif[80]; snprintf(notif,80,"Threshold set to %.2fA",gThreshold);
  addNotification(notif,"info");
  Serial.printf("[CFG] threshold=%.2fA\n",gThreshold);
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200,"application/json","{\"ok\":true}");
}

// ─── Setup ────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== Machine Guardian booting ===");

  pinMode(RED_LED,OUTPUT); pinMode(GREEN_LED,OUTPUT);
  pinMode(YELLOW_LED,OUTPUT); pinMode(BUZZER_PIN,OUTPUT);
  setLEDs(true,false,false);

  analogSetAttenuation(ADC_11db); analogReadResolution(12);
  Wire.begin(21,22);

  if(!display.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)) Serial.println("OLED not found");
  display.clearDisplay(); display.display();
  displayMessage("Machine Guardian","Booting...");

  nfc.begin();
  uint32_t ver=nfc.getFirmwareVersion();
  if(!ver){Serial.println("PN532 not found");displayMessage("NFC ERROR","Check wiring");delay(3000);}
  else    {Serial.printf("PN532 v%d.%d\n",(ver>>16)&0xFF,(ver>>8)&0xFF);nfc.SAMConfig();}

  nvsLoadEmployees();

  displayMessage("Connecting WiFi...",WIFI_SSID);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  for(int i=0;i<30&&WiFi.status()!=WL_CONNECTED;i++){delay(500);Serial.print(".");}

  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("\nIP: %s\n",WiFi.localIP().toString().c_str());
    char ip[32]; snprintf(ip,32,"%s",WiFi.localIP().toString().c_str());
    displayMessage("WiFi Connected",ip,"Open in browser");
    configTime(3*3600,0,"pool.ntp.org"); delay(1500);
  } else {
    Serial.println("\nWiFi failed"); displayMessage("WiFi Failed","Offline mode"); delay(2000);
  }

  server.on("/",                    HTTP_GET,  handleRoot);
  server.on("/api/status",          HTTP_GET,  handleStatus);
  server.on("/api/power",           HTTP_GET,  handlePower);
  server.on("/api/runtime",         HTTP_GET,  handleRuntime);
  server.on("/api/notifications",   HTTP_GET,  handleNotifications);
  server.on("/api/set_ambient",     HTTP_GET,  handleSetAmbient);
  server.on("/api/set_threshold",   HTTP_POST, handleSetThreshold);
  server.on("/api/employees",       HTTP_GET,  handleGetEmployees);
  server.on("/api/employees/add",   HTTP_POST, handleAddEmployee);
  server.on("/api/employees/remove",HTTP_POST, handleRemoveEmployee);
  server.on("/api/employees/toggle",HTTP_POST, handleToggleEmployee);
  server.on("/api/employees/clear", HTTP_POST, handleClearEmployees);
  server.begin(); Serial.println("Server started");

  displayMessage("Warming up...","");
  for(int i=0;i<20;i++){measureRawRMS();delay(30);}
  Serial.printf("Midpoint: %.1f | Ambient: %.3fA\n",gMidpoint,gAmbientAmps);

  if(gAmbientAmps==0.0f){
    displayMessage("Go to Settings","Capture ambient","before use!");
    addNotification("No ambient baseline — calibrate in Settings","warn");
    delay(3000);
  }

  addNotification("System started","info");
  enterIdle();
  Serial.println("=== Ready ===");
}

// ─── Loop ─────────────────────────────────────────────────────

void loop() {
  server.handleClient();

  float amps=measureNetAmps();
  bool  on=(amps>=gThreshold);

  if(on&&!machineOn){
    machineOn=true;
    if     (currentState==STATE_AUTHENTICATED) { stopUnauthSession(); enterRunning(amps); }
    else if(currentState==STATE_IDLE)          { enterUnauthorized(); }
  } else if(!on&&machineOn){
    machineOn=false; handleMachineOff();
  }

  // Keep tracking unauthorized runtime while machine is on without auth
  if(machineOn && currentState==STATE_IDLE && unauthRunning) {
    
  }

  if(currentState==STATE_AUTHENTICATED&&millis()-authTime>AUTH_TIMEOUT_MS){
    addNotification("Auth timeout — machine not started","warn"); enterIdle();
  }

  if(currentState==STATE_RUNNING){
    static unsigned long lastDisp=0;
    if(millis()-lastDisp>2000){
      char l2[32]; snprintf(l2,32,"%.1fA / %.0fW",amps,amps*220.0f);
      displayMessage("Machine Running",l2,currentEmployee.c_str());
      lastDisp=millis();
    }
  }

  if(millis()-lastPowerSample>=POWER_SAMPLE_INTERVAL){
    lastPowerSample=millis();
    if(currentState==STATE_RUNNING&&amps>=gThreshold){
      int idx=powerLogHead%MAX_SAMPLES;
      powerLog[idx].ts=time(nullptr); powerLog[idx].amps=amps; powerLog[idx].watts=amps*220.0f;
      powerLogHead++; if(powerLogCount<MAX_SAMPLES)powerLogCount++;
    }
  }

  checkNFC();
  delay(50);
}
