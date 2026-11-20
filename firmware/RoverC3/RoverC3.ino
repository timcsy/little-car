#include <WiFi.h>
#include <Preferences.h>
#include <WebSocketsServer.h>
#include <esp_system.h>
// Calibration logic (kept in this sketch for standalone downloads).
#include <stdint.h>
#include <math.h>

// A = left when map is 0. Direction flags 0/1 mean normal/reversed.
struct Calibration { uint8_t map, leftReverse, rightReverse; int8_t trim; };
inline bool validCalibration(const Calibration &c) {
  return c.map <= 1 && c.leftReverse <= 1 && c.rightReverse <= 1 && c.trim >= -8 && c.trim <= 8;
}
inline int clampMotor(int v) { return v < -100 ? -100 : v > 100 ? 100 : v; }
inline int scaledMotor(int v, int trimSteps) {
  // Match JavaScript Math.round, including negative half values.
  return (int)floor((double)v * (1.0 - trimSteps * 0.05) + 0.5);
}
inline void calibrateMotors(const Calibration &c, int left, int right, int trim, int &a, int &b) {
  left=clampMotor(left); right=clampMotor(right);
  if(trim>0) right=scaledMotor(right,trim); else if(trim<0) left=scaledMotor(left,-trim);
  if(c.leftReverse) left=-left; if(c.rightReverse) right=-right;
  a=c.map ? right : left; b=c.map ? left : right;
}

// End calibration logic

// DRV8833 IN1, IN2, IN3, IN4
constexpr int pins[] = {4, 3, 1, 0};
constexpr unsigned MOTOR_TIMEOUT_MS = 650, OWNER_TIMEOUT_MS = 3000;
Preferences prefs;
WebSocketsServer ws(81);
bool authorized[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
bool calibratedClient[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
Calibration calibration={0,0,0,0};
uint32_t calibrationRevision=1;
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
String calibrationMessage() { return "CAL:"+String(calibrationRevision)+","+String(calibration.map)+","+String(calibration.leftReverse)+","+String(calibration.rightReverse)+","+String((int)calibration.trim); }
void reportCalibration() { String msg=calibrationMessage(); for(int i=0;i<WEBSOCKETS_SERVER_CLIENT_MAX;i++) if(authorized[i]) ws.sendTXT(i,msg); }
void loadCalibration() {
  uint8_t bytes[5];
  if(prefs.getBytesLength("cal")!=sizeof(bytes) || prefs.getBytes("cal",bytes,sizeof(bytes))!=sizeof(bytes) || bytes[0]!=1 || bytes[4]>16) return;
  Calibration candidate={bytes[1],bytes[2],bytes[3],(int8_t)((int)bytes[4]-8)};
  if(validCalibration(candidate)) calibration=candidate;
}
bool saveCalibration(const Calibration &c) {
  uint8_t bytes[5]={1,c.map,c.leftReverse,c.rightReverse,(uint8_t)(c.trim+8)};
  return prefs.putBytes("cal",bytes,sizeof(bytes))==sizeof(bytes);
}

void reportSerial() {
  String savedSsid=prefs.getString("ssid", "");
  if (savedSsid.length()) { Serial.print("WIFIINFO\t"); Serial.print(savedSsid); Serial.print('\t'); Serial.println(prefs.getString("password", "").length() ? "1" : "0"); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("PAIR\t"); Serial.print(WiFi.localIP()); Serial.print('\t'); Serial.println(pairCode);
  } else Serial.println("STATUS\tDISCONNECTED");
}
void event(uint8_t n, WStype_t type, uint8_t *payload, size_t length) {
  if (n >= WEBSOCKETS_SERVER_CLIENT_MAX) return;
  if (type == WStype_CONNECTED) { authorized[n] = false; calibratedClient[n] = false; return; }
  if (type == WStype_DISCONNECTED) { authorized[n] = false; calibratedClient[n] = false; if (shareSender == n) shareAbort(); if (owner == n) releaseOwner(); return; }
  if (type != WStype_TEXT || length > 1100) return;
  String msg; msg.reserve(length); for (size_t i=0; i<length; ++i) msg += char(payload[i]);
  if (!authorized[n]) {
    if (msg == "AUTH:" + pairCode) { authorized[n] = true; ws.sendTXT(n, "AUTH:OK"); ws.sendTXT(n, "CAPS:SHARE1"); ws.sendTXT(n, "CAPS:CAL1"); {String cal=calibrationMessage();ws.sendTXT(n,cal);} reportState(); }
    else { ws.sendTXT(n, "AUTH:FAIL"); ws.disconnect(n); }
    return;
  }
  if (msg == "ESTOP") { shareAbort(); releaseOwner(); return; }
  if (msg == "CALCLIENT:1") { calibratedClient[n]=true; String cal=calibrationMessage();ws.sendTXT(n,cal);reportState();return; }
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
  if (msg == "CLAIM") { if (!calibratedClient[n]) {ws.sendTXT(n,"CAL:UPGRADE");ws.disconnect(n);return;} if (owner < 0 && shareSender < 0) { motorStop(); owner=n; lastOwner=millis(); lastMotor=0; } reportState(); return; }
  if (msg == "RELEASE") { if (owner == n) releaseOwner(); else reportState(); return; }
  if (n != owner) return;
  if (msg == "PING") { lastOwner=millis(); return; }
  if (!calibratedClient[n]) return;
  if (msg.startsWith("CALSET:")) {
    unsigned long req=0,rev=0;int m=-1,l=-1,r=-1,t=-99,used=0;
    if(sscanf(msg.c_str(),"CALSET:%lu,%lu,%d,%d,%d,%d%n",&req,&rev,&m,&l,&r,&t,&used)!=6 || used!=(int)msg.length() || req==0 || m<0 || m>1 || l<0 || l>1 || r<0 || r>1 || t< -8 || t>8) {ws.sendTXT(n,"CALERROR:FORMAT");return;}
    motorStop();lastMotor=0;lastOwner=millis();
    if(rev!=calibrationRevision){String cal=calibrationMessage();ws.sendTXT(n,cal);ws.sendTXT(n,"CALERROR:CONFLICT");return;}
    Calibration candidate={(uint8_t)m,(uint8_t)l,(uint8_t)r,(int8_t)t};
    if(!saveCalibration(candidate)){ws.sendTXT(n,"CALERROR:STORAGE");return;}
    calibration=candidate;calibrationRevision++;reportCalibration();ws.sendTXT(n,"CALSAVED:"+String(req));return;
  }
  bool raw=msg.startsWith("R:"), logical=msg.startsWith("L:"), preview=msg.startsWith("P:");
  if(raw||logical||preview) {
    int x=0,y=0,t=0,used=0, parsed=preview?sscanf(msg.c_str(),"P:%d,%d,%d%n",&x,&y,&t,&used):sscanf(msg.c_str(),raw?"R:%d,%d%n":"L:%d,%d%n",&x,&y,&used);
    if(parsed!=(preview?3:2) || used!=(int)msg.length() || x< -100 || x>100 || y< -100 || y>100 || (preview&&(t< -8 || t>8))) return;
    int a=x,b=y;if(!raw)calibrateMotors(calibration,x,y,preview?t:calibration.trim,a,b);
    lastOwner=lastMotor=millis();drive(a,b);
  }
}
void handleSerial(String line) {
  if (line == "STATUS") { reportSerial(); return; }
  if (line == "WIFIREUSE") {
    String ssid=prefs.getString("ssid", ""), password=prefs.getString("password", "");
    if (!ssid.length()) { Serial.println("ERROR\tWIFI"); return; }
    shareAbort(); releaseOwner(); ws.disconnect(); for(bool &a:authorized)a=false;
    Serial.println("STATUS\tCONNECTING"); WiFi.disconnect(); WiFi.begin(ssid.c_str(),password.c_str()); return;
  }
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
  motorStop(); Serial.begin(115200); prefs.begin("rover", false); loadCalibration();
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
