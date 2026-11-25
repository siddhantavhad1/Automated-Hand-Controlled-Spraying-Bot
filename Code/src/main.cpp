/*
 * ROVER CONTROL v5 - 4 INDEPENDENT MOTORS, NO EXTRA BOARDS  (ESP8266)
 *
 * Each motor gets its own two ESP pins, so all four can run in any
 * combination at the same time. No I/O expander needed.
 *
 *            FRONT
 *      M1 (FL)    M3 (FR)
 *      M2 (RL)    M4 (RR)
 *            REAR
 *
 * WIRING (ESP pin -> L298N input)
 *   M1:  D1 -> L298N #1 IN1    D2 -> L298N #1 IN2    (motor on OUT1/OUT2)
 *   M2:  D5 -> L298N #1 IN3    D6 -> L298N #1 IN4    (motor on OUT3/OUT4)
 *   M3:  D3 -> L298N #2 IN1    D4 -> L298N #2 IN2    (motor on OUT1/OUT2)
 *   M4:  D8 -> L298N #2 IN3    RX -> L298N #2 IN4    (motor on OUT3/OUT4)
 *   ENA and ENB jumpers must be ON for BOTH drivers (4 jumpers in total).
 *
 *   Pump relay  -> D7     Light relay -> D0     (active LOW)
 *   12V 5A supply -> fuse -> both L298N 12V terminals + buck converter (5V for ESP)
 *   Common GND for everything.
 *
 * NOTE: unplug the 12V and the RX wire while uploading code over USB.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ---------------- CONFIG ----------------
const char*   WIFI_SSID    = "MC FARM";
const char*   WIFI_PASS    = "9876543210";     // 8+ chars
const uint8_t WIFI_CHANNEL = 6;
const uint8_t MAX_CLIENTS  = 4;

// Motor input pins: every motor has two pins (IN_A = forward, IN_B = reverse)
//                          M1   M2   M3   M4
const uint8_t IN_A[4] = {   5,  14,   0,  15 };   // D1, D5, D3, D8
const uint8_t IN_B[4] = {   4,  12,   2,   3 };   // D2, D6, D4, RX

const uint8_t PUMP_PIN  = 13;    // D7
const uint8_t LIGHT_PIN = 16;    // D0 (HIGH at boot = relay OFF)
const uint8_t RELAY_ON  = LOW;
const uint8_t RELAY_OFF = HIGH;

// Per-motor spin correction. If a wheel turns backward on FORWARD, set -1.
// Index: 0 = M1 (FL), 1 = M2 (RL), 2 = M3 (FR), 3 = M4 (RR)
const int8_t POLARITY[4] = { 1, 1, -1, -1 };

// Failsafe: stop all motors if the page stops sending (phone locked, WiFi lost)
const unsigned long MOVE_TIMEOUT_MS = 700;
// ----------------------------------------

struct State {
    bool systemOn = false;
    bool pump     = false;
    bool light    = false;
    int8_t dir[4] = { 0, 0, 0, 0 };   // logical direction per motor: 1, -1, 0
    unsigned long lastMoveMs = 0;
} st;

ESP8266WebServer server(80);

// ---------------- HARDWARE ----------------
void applyMotors()
{
    for (uint8_t i = 0; i < 4; i++) {
        int8_t d = st.dir[i] * POLARITY[i];
        digitalWrite(IN_A[i], d > 0 ? HIGH : LOW);
        digitalWrite(IN_B[i], d < 0 ? HIGH : LOW);
    }
}

bool anyMoving()
{
    for (uint8_t i = 0; i < 4; i++) if (st.dir[i] != 0) return true;
    return false;
}

void stopAll()
{
    for (uint8_t i = 0; i < 4; i++) st.dir[i] = 0;
    applyMotors();
}

// Set ALL four motors in one go, so any combination runs at the same time
void setAll(const int8_t d[4])
{
    bool run = false;
    for (uint8_t i = 0; i < 4; i++) {
        st.dir[i] = st.systemOn ? d[i] : 0;
        if (st.dir[i] != 0) run = true;
    }
    if (run) st.lastMoveMs = millis();
    applyMotors();
}

void setPump(bool on)
{
    st.pump = on && st.systemOn;
    digitalWrite(PUMP_PIN, st.pump ? RELAY_ON : RELAY_OFF);
}

void setLight(bool on)
{
    st.light = on && st.systemOn;
    digitalWrite(LIGHT_PIN, st.light ? RELAY_ON : RELAY_OFF);
}

void setSystem(bool on)
{
    st.systemOn = on;
    stopAll();
    if (!on) { setPump(false); setLight(false); }
}

void allSafe()
{
    stopAll();
    setPump(false);
    setLight(false);
}

// ---------------- WEB UI ----------------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>ROVER</title>
<style>
*{box-sizing:border-box}
body{background:#111;color:#fff;font-family:Arial,sans-serif;text-align:center;margin:0;padding:16px;
     -webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
h1{font-size:26px;margin:6px 0}
h2{font-size:15px;color:#aaa;margin:22px 0 6px;letter-spacing:2px}
button{width:104px;height:58px;margin:5px;border:0;border-radius:14px;font-size:16px;
       font-weight:bold;color:#fff;touch-action:none}
button:active{filter:brightness(1.4)}
.mv{background:#2196F3}.st{background:#F44336}.on{background:#4CAF50}
.off{background:#555}.dev{background:#FF9800}
.mt{background:#7E57C2;width:62px;height:48px;margin:3px;font-size:13px}
.mt.z{background:#555}
.lit{outline:3px solid #fff;filter:brightness(1.25)}
#bar{display:flex;gap:8px;justify-content:center;margin:8px 0;flex-wrap:wrap}
.pill{padding:6px 12px;border-radius:20px;background:#333;font-size:13px}
.pill.a{background:#4CAF50}.pill.bad{background:#c62828}
#conn{color:#f66;font-size:12px;height:16px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;max-width:420px;margin:0 auto}
.box{background:#1c1c1c;border-radius:12px;padding:6px}
.box b{font-size:13px;color:#ccc}
small{color:#888}
</style></head><body>
<h1>ROVER CONTROL</h1>
<div id="conn"></div>
<div id="bar">
  <span id="p_sys" class="pill">SYSTEM</span>
  <span id="p_pump" class="pill">PUMP</span>
  <span id="p_light" class="pill">LIGHTS</span>
</div>

<h2>SYSTEM</h2>
<button class="on" onclick="cmd('systemon')">ON</button>
<button class="off" onclick="cmd('systemoff')">OFF</button>

<h2>ALL 4 MOTORS (HOLD)</h2>
<div><button id="fwd" class="mv">FORWARD</button></div>
<div>
  <button id="left" class="mv">LEFT</button>
  <button class="st" onclick="stopAll()">STOP ALL</button>
  <button id="right" class="mv">RIGHT</button>
</div>
<div><button id="rev" class="mv">REVERSE</button></div>

<h2>EACH MOTOR (TAP TO RUN, TAP AGAIN TO STOP)</h2>
<small>Run any combination, all four can be on together</small>
<div class="grid">
  <div class="box"><b>M1 Front Left</b><br>
    <button id="m0r" class="mt" onclick="tog(0,-1)">REV</button><button id="m0s" class="mt z" onclick="tog(0,0)">STOP</button><button id="m0f" class="mt" onclick="tog(0,1)">FWD</button></div>
  <div class="box"><b>M3 Front Right</b><br>
    <button id="m2r" class="mt" onclick="tog(2,-1)">REV</button><button id="m2s" class="mt z" onclick="tog(2,0)">STOP</button><button id="m2f" class="mt" onclick="tog(2,1)">FWD</button></div>
  <div class="box"><b>M2 Rear Left</b><br>
    <button id="m1r" class="mt" onclick="tog(1,-1)">REV</button><button id="m1s" class="mt z" onclick="tog(1,0)">STOP</button><button id="m1f" class="mt" onclick="tog(1,1)">FWD</button></div>
  <div class="box"><b>M4 Rear Right</b><br>
    <button id="m3r" class="mt" onclick="tog(3,-1)">REV</button><button id="m3s" class="mt z" onclick="tog(3,0)">STOP</button><button id="m3f" class="mt" onclick="tog(3,1)">FWD</button></div>
</div>

<h2>PUMP</h2>
<button class="dev" onclick="cmd('pumpon')">ON</button>
<button class="off" onclick="cmd('pumpoff')">OFF</button>

<h2>LIGHTS</h2>
<button class="dev" onclick="cmd('lightson')">ON</button>
<button class="off" onclick="cmd('lightsoff')">OFF</button>

<script>
// md = wanted direction of each motor [M1,M2,M3,M4]; the whole array is sent every time,
// so all four motors always get their command together.
var md=[0,0,0,0], hb=null;

function anyOn(){ return md.some(function(v){return v!==0;}); }
function pushMd(){ return fetch('/cmd?c=motors&v='+md.join(',')).catch(function(){}); }

function setMd(a){
  md=a.slice();
  pushMd();
  render();
  if(anyOn()){ if(!hb) hb=setInterval(pushMd,250); }       // keep-alive while anything runs
  else if(hb){ clearInterval(hb); hb=null; }
}
function tog(i,d){ var a=md.slice(); a[i]=(a[i]===d)?0:d; setMd(a); }
function stopAll(){ setMd([0,0,0,0]); fetch('/cmd?c=stop').catch(function(){}); }

function cmd(q){
  return fetch('/cmd?c='+q).then(poll).catch(function(){});
}

// Hold-to-run for the 4 group buttons
function hold(id,pattern){
  var b=document.getElementById(id), on=false;
  function s(e){ e.preventDefault(); on=true; setMd(pattern); }
  function x(e){ e.preventDefault(); if(on){ on=false; setMd([0,0,0,0]); } }
  b.addEventListener('pointerdown',s);
  ['pointerup','pointerleave','pointercancel'].forEach(function(ev){b.addEventListener(ev,x);});
  b.addEventListener('contextmenu',function(e){e.preventDefault();});
}
hold('fwd',[1,1,1,1]);  hold('rev',[-1,-1,-1,-1]);
hold('left',[-1,-1,1,1]); hold('right',[1,1,-1,-1]);

function lit(id,on){ document.getElementById(id).classList.toggle('lit',on); }
function eq(a,b){ for(var i=0;i<4;i++) if(a[i]!==b[i]) return false; return true; }

function render(){
  lit('fwd',eq(md,[1,1,1,1]));    lit('rev',eq(md,[-1,-1,-1,-1]));
  lit('left',eq(md,[-1,-1,1,1])); lit('right',eq(md,[1,1,-1,-1]));
  for(var i=0;i<4;i++){
    lit('m'+i+'f',md[i]===1); lit('m'+i+'r',md[i]===-1); lit('m'+i+'s',md[i]===0);
  }
}

function poll(){
  return fetch('/status').then(function(r){return r.json();}).then(function(s){
    document.getElementById('conn').textContent='';
    document.getElementById('p_sys').className='pill'+(s.sys?' a':'');
    document.getElementById('p_pump').className='pill'+(s.pump?' a':'');
    document.getElementById('p_light').className='pill'+(s.light?' a':'');
    if(!s.sys && anyOn()) setMd([0,0,0,0]);     // system turned off: clear everything
  }).catch(function(){
    document.getElementById('conn').textContent='Connection lost...';
    md=[0,0,0,0]; if(hb){clearInterval(hb);hb=null;} render();
  });
}
setInterval(poll,1000); poll(); render();
</script></body></html>
)rawliteral";

void handleStatus()
{
    String j = "{\"sys\":";   j += st.systemOn ? "true" : "false";
    j += ",\"pump\":";        j += st.pump  ? "true" : "false";
    j += ",\"light\":";       j += st.light ? "true" : "false";
    j += ",\"m\":[";
    for (uint8_t i = 0; i < 4; i++) { j += st.dir[i]; if (i < 3) j += ","; }
    j += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

// Parse "1,0,-1,1" into 4 directions. Returns false if the text is invalid.
bool parseMotors(const String& v, int8_t out[4])
{
    int start = 0;
    for (uint8_t i = 0; i < 4; i++) {
        int comma = v.indexOf(',', start);
        if (i < 3 && comma < 0) return false;
        String part = (i < 3) ? v.substring(start, comma) : v.substring(start);
        if (part.length() == 0) return false;
        int d = part.toInt();
        if (d < -1 || d > 1) return false;
        out[i] = d;
        start = comma + 1;
    }
    return true;
}

void handleCmd()
{
    String c = server.arg("c");

    if      (c == "systemon")  setSystem(true);
    else if (c == "systemoff") setSystem(false);
    else if (c == "stop")      stopAll();
    else if (c == "pumpon")    setPump(true);
    else if (c == "pumpoff")   setPump(false);
    else if (c == "lightson")  setLight(true);
    else if (c == "lightsoff") setLight(false);
    else if (c == "motors") {
        int8_t d[4];
        if (!parseMotors(server.arg("v"), d)) { server.send(400, "text/plain", "BAD"); return; }
        setAll(d);
    }
    else { server.send(400, "text/plain", "BAD"); return; }

    server.send(200, "text/plain", "OK");
}

// ---------------- SETUP / LOOP ----------------
void setup()
{
    pinMode(PUMP_PIN, OUTPUT);
    pinMode(LIGHT_PIN, OUTPUT);
    digitalWrite(PUMP_PIN, RELAY_OFF);
    digitalWrite(LIGHT_PIN, RELAY_OFF);

    // TX-only serial, so RX (GPIO3) is free to be a motor pin
    Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);

    for (uint8_t i = 0; i < 4; i++) {
        pinMode(IN_A[i], OUTPUT);
        pinMode(IN_B[i], OUTPUT);
    }
    stopAll();
    Serial.println("\nRover ready");

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setOutputPower(17);
    WiFi.softAP(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, 0, MAX_CLIENTS);

    Serial.printf("AP \"%s\"  IP: %s\n", WIFI_SSID, WiFi.softAPIP().toString().c_str());

    server.on("/",       []{ server.send_P(200, "text/html", PAGE); });
    server.on("/cmd",    handleCmd);
    server.on("/status", handleStatus);
    server.onNotFound([]{ server.send(404, "text/plain", "Not found"); });
    server.begin();
}

void loop()
{
    server.handleClient();

    // Failsafe: commands went stale (page closed / signal lost)
    if (anyMoving() && millis() - st.lastMoveMs > MOVE_TIMEOUT_MS) {
        stopAll();
    }

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 500) {
        lastCheck = millis();

        // Failsafe: nobody connected to the rover
        if (WiFi.softAPgetStationNum() == 0 && (anyMoving() || st.pump || st.light)) {
            allSafe();
        }
    }
}