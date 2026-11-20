#include <WiFi.h>
#include <Preferences.h>
#include <WebSocketsServer.h>
#include <esp_system.h>

// DRV8833 IN1, IN2, IN3, IN4
constexpr int pins[] = {4, 3, 1, 0};
constexpr unsigned MOTOR_TIMEOUT_MS = 650, OWNER_TIMEOUT_MS = 3000;
Preferences prefs;
WebSocketsServer ws(81);
bool authorized[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
int owner = -1;
unsigned long lastOwner = 0, lastMotor = 0;
String pairCode, serialLine;
bool wasConnected = false;
int shareSender = -1;
unsigned long shareLast = 0;
unsigned shareNext = 0, shareTotal = 0;
String shareId;
void shareBroadcast(String message, int except = -1) {
  for (int i=0; i<WEBSOCKETS_SERVER_CLIENT_MAX; ++i) if (authorized[i] && i!=except) ws.sendTXT(i, message);
}
void reportState();
void shareAbort() {
  if (shareSender < 0) return;
  shareBroadcast("SHARE:ABORT:" + shareId);
  shareSender=-1; shareId=""; shareNext=shareTotal=0; reportState();
}

void motorStop() { for (int p : pins) ledcWrite(p, 0); }
void drive(int a, int b) {
  a = constrain(a, -100, 100); b = constrain(b, -100, 100);
  // Stop both bridge inputs before changing polarity.
  motorStop();
  int values[] = {a, b};
  for (int i = 0; i < 2; ++i) {
    int v = values[i];
    if (v) ledcWrite(pins[2*i + (v < 0 ? 1 : 0)], map(abs(v), 0, 100, 0, 255));
  }
}
void reportState() {
  for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (authorized[i]) ws.sendTXT(i, owner == i ? "STATE:OWNER" : owner < 0 ? "STATE:FREE" : "STATE:BUSY");
  }
}
void releaseOwner() { motorStop(); owner = -1; lastMotor = 0; reportState(); }
void reportSerial() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("PAIR\t"); Serial.print(WiFi.localIP()); Serial.print('\t'); Serial.println(pairCode);
  } else Serial.println("STATUS\tDISCONNECTED");
}
void event(uint8_t n, WStype_t type, uint8_t *payload, size_t length) {
  if (n >= WEBSOCKETS_SERVER_CLIENT_MAX) return;
  if (type == WStype_CONNECTED) { authorized[n] = false; return; }
  if (type == WStype_DISCONNECTED) { authorized[n] = false; if (shareSender == n) shareAbort(); if (owner == n) releaseOwner(); return; }
  if (type != WStype_TEXT || length > 1100) return;
  String msg; msg.reserve(length); for (size_t i=0; i<length; ++i) msg += char(payload[i]);
  if (!authorized[n]) {
    if (msg == "AUTH:" + pairCode) { authorized[n] = true; ws.sendTXT(n, "AUTH:OK"); ws.sendTXT(n, "CAPS:SHARE1"); reportState(); }
    else { ws.sendTXT(n, "AUTH:FAIL"); ws.disconnect(n); }
    return;
  }
  if (msg == "ESTOP") { shareAbort(); releaseOwner(); return; }
  if (msg.startsWith("SHARE:BEGIN:")) {
    if (shareSender>=0 || length>100) { ws.sendTXT(n,"SHARE:ERROR"); return; }
    int a=msg.indexOf(':',12), b=msg.indexOf(':',a+1);
    String id=msg.substring(12,a), count=msg.substring(a+1,b), size=msg.substring(b+1);
    if(a<0 || b<0 || id.length()!=12 || count.length()>3 || size.length()>6) {ws.sendTXT(n,"SHARE:ERROR");return;}
    for (unsigned i=0;i<id.length();i++) if (!isHexadecimalDigit(id[i])) {ws.sendTXT(n,"SHARE:ERROR");return;}
    for (unsigned i=0;i<count.length();i++) if (!isDigit(count[i])) {ws.sendTXT(n,"SHARE:ERROR");return;}
    for (unsigned i=0;i<size.length();i++) if (!isDigit(size[i])) {ws.sendTXT(n,"SHARE:ERROR");return;}
    unsigned total=count.toInt(), bytes=size.toInt(), peers=0;
    for(int i=0;i<WEBSOCKETS_SERVER_CLIENT_MAX;i++) if(authorized[i] && i!=n) peers++;
    if(!peers || !total || total>220 || !bytes || bytes>131072) {ws.sendTXT(n,"SHARE:ERROR");return;}
    releaseOwner(); shareSender=n; shareId=id; shareTotal=total; shareNext=0; shareLast=millis();
    shareBroadcast(msg,n); ws.sendTXT(n,"SHARE:ACK:"+id+":BEGIN"); return;
  }
  if (msg.startsWith("SHARE:CHUNK:")) {
    if (n!=shareSender) return;
    int a=msg.indexOf(':',12), b=msg.indexOf(':',a+1);
    if(a<0 || b<0 || msg.substring(12,a)!=shareId || msg.substring(a+1,b)!=String(shareNext) || msg.length()-b-1>800 || msg.length()==b+1) {shareAbort();return;}
    for(unsigned i=b+1;i<msg.length();i++) {char c=msg[i];if(!isalnum(c) && c!='+' && c!='/' && c!='='){shareAbort();return;}}
    shareBroadcast(msg,n); ws.sendTXT(n,"SHARE:ACK:"+shareId+":"+String(shareNext)); shareNext++; shareLast=millis(); return;
  }
  if (msg.startsWith("SHARE:END:")) {
    if(n==shareSender && msg.substring(10)==shareId && shareNext==shareTotal) {shareBroadcast(msg,n);ws.sendTXT(n,"SHARE:ACK:"+shareId+":END");shareSender=-1;shareId="";reportState();} return;
  }
  if (msg.startsWith("SHARE:CANCEL:")) {if(n==shareSender) shareAbort();return;}
  if (length>100) return;
  if (msg == "STOP" && n == owner) { motorStop(); lastMotor=0; ws.sendTXT(n,"STOPPED"); return; }
  if (msg == "CLAIM") { if (owner < 0 && shareSender < 0) { motorStop(); owner=n; lastOwner=millis(); lastMotor=0; } reportState(); return; }
  if (msg == "RELEASE") { if (owner == n) releaseOwner(); else reportState(); return; }
  if (n != owner) return;
  if (msg == "PING") { lastOwner=millis(); return; }
  if (msg.startsWith("M:")) {
    int comma = msg.indexOf(',',2); if (comma < 0) return;
    String x=msg.substring(2,comma), y=msg.substring(comma+1);
    if (!x.length() || !y.length() || x.length()>4 || y.length()>4) return;
    for (unsigned i=0;i<x.length();i++) if (!(isDigit(x[i]) || (i==0 && x[i]=='-'))) return;
    for (unsigned i=0;i<y.length();i++) if (!(isDigit(y[i]) || (i==0 && y[i]=='-'))) return;
    lastOwner=lastMotor=millis(); drive(x.toInt(),y.toInt());
  }
}
void handleSerial(String line) {
  if (line == "STATUS") { reportSerial(); return; }
  if (line.startsWith("WIFI\t")) {
    int split=line.indexOf('\t',5); if (split<0) { Serial.println("ERROR\tFORMAT"); return; }
    String ssid=line.substring(5,split), password=line.substring(split+1);
    if (!ssid.length() || ssid.length()>32 || password.length()>63) { Serial.println("ERROR\tWIFI"); return; }
    shareAbort(); releaseOwner(); ws.disconnect(); for(bool &a:authorized)a=false;
    prefs.putString("ssid",ssid); prefs.putString("password",password);
    Serial.println("STATUS\tCONNECTING"); WiFi.disconnect(); WiFi.begin(ssid.c_str(),password.c_str());
  }
}
void setup() {
  for (int p : pins) { pinMode(p,OUTPUT); digitalWrite(p,LOW); ledcAttach(p, 12000, 8); }
  motorStop(); Serial.begin(115200); prefs.begin("rover", false);
  pairCode=prefs.getString("pair",""); if(pairCode.length()!=6){char code[7]; snprintf(code,sizeof(code),"%06u",(unsigned)(esp_random()%1000000));pairCode=code;prefs.putString("pair",pairCode);}
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); String ssid=prefs.getString("ssid",""); if(ssid.length()){String pw=prefs.getString("password","");WiFi.begin(ssid.c_str(),pw.c_str());}
  ws.begin(); ws.onEvent(event); Serial.println("STATUS\tREADY");
}
void loop() {
  ws.loop();
  while(Serial.available()) { char c=Serial.read(); if(c=='\n'){handleSerial(serialLine);serialLine="";} else if(c!='\r' && serialLine.length()<160)serialLine+=c; }
  bool connected=WiFi.status()==WL_CONNECTED;
  if(connected != wasConnected){wasConnected=connected;if(connected)reportSerial();else {shareAbort();releaseOwner();ws.disconnect();for(bool &a:authorized)a=false;Serial.println("STATUS\tDISCONNECTED");}}
  unsigned long now=millis();
  if(shareSender>=0 && (unsigned long)(now-shareLast)>10000) shareAbort();
  if(owner>=0 && (unsigned long)(now-lastOwner)>OWNER_TIMEOUT_MS) releaseOwner();
  if(owner>=0 && lastMotor && (unsigned long)(now-lastMotor)>MOTOR_TIMEOUT_MS) releaseOwner();
  delay(1);
}
