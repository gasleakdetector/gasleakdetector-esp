#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

// Manages WiFi connectivity and the captive-portal configuration UI.
//
// On init the device starts as an AP + STA simultaneously.
// A DNS server redirects all queries to the AP IP so any HTTP request
// from a connected client opens the config page (captive portal pattern).
// The web UI is streamed in small PROGMEM chunks to avoid allocating a
// large String on the heap (which would cause crashes on ESP8266).

#include "Config.h"

class WiFiManager {
private:
  DeviceConfig  config;
  String        apName;
  bool          apStarted;
  unsigned long lastConnectionCheck;
  unsigned long connectStartTime;
  bool          isConnecting;
  IPAddress     apIP;
  IPAddress     netMask;
  ESP8266WebServer* server;
  DNSServer*        dns;
  static const unsigned long CONNECTION_CHECK_INTERVAL = 5000;

  String buildAPName() { return String(DEVICE_ID); }

  void startDNS() {
    if (!dns) {
      dns = new DNSServer();
      dns->setErrorReplyCode(DNSReplyCode::NoError);
      dns->start(DNS_PORT, "*", apIP);
    }
  }

  void stopDNS() {
    if (dns) { dns->stop(); delete dns; dns = nullptr; }
  }

  void setupWebServer() {
    if (!server) server = new ESP8266WebServer(80);

    server->on("/", [this]() {
      sendPageChunked();
    });
    server->on("/scan", [this]() {
      WiFi.scanNetworks(true);
      server->send(200, "text/plain", "ok");
    });
    server->on("/scanresult", [this]() {
      server->send(200, "application/json", scanNetworks());
    });
    server->on("/save", [this]() {
      String r = saveConfig();
      r == "OK" ? server->send(200, "text/plain", "OK")
                : server->send(400, "text/plain", r);
    });
    server->on("/savesettings", [this]() {
      String r = saveSettings();
      r == "OK" ? server->send(200, "text/plain", "OK")
                : server->send(400, "text/plain", r);
    });
    server->on("/getsettings", [this]() {
      DeviceParams p = resolveParams(config.params);
      String json = "{";
      json += "\"apiUrl\":\""       + String(config.apiUrl)   + "\",";
      json += "\"sendInterval\":" + String(p.sendInterval)  + ",";
      json += "\"ppmWarning\":" + String(p.ppmWarning)    + ",";
      json += "\"ppmDanger\":" + String(p.ppmDanger)     + ",";
      json += "\"ppmBuzzer\":" + String(p.ppmBuzzer)     + ",";
      json += "\"deviceId\":\""    + String(config.deviceId) + "\",";
      json += "\"name\":\""        + String(config.name)     + "\",";
      json += "\"lat\":" + String(config.latitude,  6)        + ",";
      json += "\"lng\":" + String(config.longitude, 6);
      json += "}";
      server->send(200, "application/json", json);
    });
    server->on("/status", [this]() {
      server->send(200, "application/json", buildStatusJson());
    });
    server->on("/generate_204", [this]() { handleCaptivePortal(); });
    server->on("/fwlink",       [this]() { handleCaptivePortal(); });
    server->onNotFound(         [this]() { handleCaptivePortal(); });
    server->begin();
  }

  String buildStatusJson() {
    String s = "{\"ap\":\"" + apName + "\",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
    s += ",\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    s += ",\"wl_status\":"  + String(WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
      s += ",\"sta_ip\":\"" + WiFi.localIP().toString() + "\"";
      s += ",\"ssid\":\""   + String(config.ssid) + "\"";
    }
    s += "}";
    return s;
  }

  void handleCaptivePortal() {
    if (!isIPAddress(server->hostHeader())) {
      server->sendHeader("Location", String("http://") + ipToString(apIP), true);
      server->send(302, "text/plain", "");
    } else {
      sendPageChunked();
    }
  }

  bool isIPAddress(const String& str) {
    for (size_t i = 0; i < str.length(); i++) {
      int c = str.charAt(i);
      if (c != '.' && (c < '0' || c > '9')) return false;
    }
    return true;
  }

  String ipToString(const IPAddress& ip) {
    String r;
    for (int i = 0; i < 4; i++) {
      r += String((ip >> (8 * i)) & 0xFF);
      if (i < 3) r += '.';
    }
    return r;
  }

  // Send page in small PROGMEM chunks — avoids allocating a 12KB String on heap.
  // Each chunk is a static const char[] in Flash; only ~256B copied to stack at a time.
  // This is the correct pattern for ESP8266 with limited heap.
  void sendPageChunked() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/html", "");

    // ── <head> + CSS ────────────────────────────────────────────────
    server->sendContent_P(PSTR(
      "<!DOCTYPE html><html><head>"
      "<meta charset=\"UTF-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Gas Leak Setup</title>"
      "<style>"
      "*{margin:0;padding:0;box-sizing:border-box}"
      "body{font-family:Arial;background:#111;color:#eee;min-height:100vh;display:flex;align-items:flex-start;justify-content:center;padding:20px}"
      ".card{width:100%;max-width:400px;background:#1e1e1e;border-radius:12px;overflow:hidden}"
      ".header{padding:20px 20px 16px;text-align:center;position:relative;border-bottom:1px solid #2a2a2a}"
      ".gear-btn{position:absolute;top:16px;right:16px;background:none;border:none;color:#777;cursor:pointer;font-size:20px;padding:6px;line-height:1;border-radius:8px;transition:color .2s,background .2s}"
      ".gear-btn:hover{color:#4CAF50;background:#2a2a2a}"
      ".gear-btn.active{color:#4CAF50}"
      "h2{color:#4CAF50;font-size:22px}"
      ".body{padding:20px}"
      ".panel{display:none}.panel.active{display:block}"
    ));
    yield();

    server->sendContent_P(PSTR(
      ".info{background:#0d3d4f;border-left:3px solid #4CAF50;padding:10px 12px;border-radius:4px;margin-bottom:18px;font-size:13px;line-height:1.5;color:#ccc}"
      ".fg{margin-bottom:16px}"
      "label{display:block;margin-bottom:6px;font-size:13px;color:#aaa;font-weight:500}"
      "input,select{width:100%;padding:11px 12px;border:1px solid #333;background:#2a2a2a;color:#eee;border-radius:8px;font-size:14px;outline:none;transition:border .2s}"
      "input:focus,select:focus{border-color:#4CAF50}"
      "select{-webkit-appearance:none;appearance:none;"
        "background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath fill='%23888' d='M6 8L0 0h12z'/%3E%3C/svg%3E\");"
        "background-repeat:no-repeat;background-position:right 12px center}"
      ".scan-row{display:flex;align-items:center;gap:8px;margin-bottom:8px}"
      ".scan-row select{flex:1}"
      ".scan-btn{flex-shrink:0;width:42px;height:42px;background:#2a2a2a;border:1px solid #333;color:#eee;border-radius:8px;cursor:pointer;font-size:18px;display:flex;align-items:center;justify-content:center;transition:background .2s}"
      ".scan-btn:hover{background:#333}"
      ".scan-btn:disabled{opacity:.4;cursor:not-allowed}"
      ".scan-status{font-size:12px;color:#777;min-height:16px;margin-bottom:4px}"
    ));
    yield();

    server->sendContent_P(PSTR(
      ".hint{font-size:11px;color:#666;margin-top:4px}"
      ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
      "input[type=number]{-moz-appearance:textfield}"
      "input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none}"
      ".btn{display:block;width:100%;padding:13px;border:none;border-radius:8px;cursor:pointer;font-size:15px;font-weight:600;margin-top:8px;transition:opacity .2s}"
      ".btn:active{opacity:.8}"
      ".btn:disabled{opacity:.5;cursor:not-allowed}"
      ".btn-primary{background:#4CAF50;color:#fff}"
      ".btn-sec{background:#2a2a2a;color:#aaa;border:1px solid #333}"
      ".msg{margin-top:14px;padding:11px 14px;border-radius:8px;font-size:13px;text-align:center;display:none;line-height:1.4}"
      ".msg-ok{background:#1b3a1e;color:#81c784;border:1px solid #2e7d32}"
      ".msg-err{background:#3a1a1a;color:#ef9a9a;border:1px solid #7d2e2e}"
      ".msg-warn{background:#3a2e1a;color:#ffcc80;border:1px solid #7d5c2e}"
      ".dots::after{content:'.';animation:dots 1.2s steps(3,end) infinite}"
      "@keyframes dots{0%{content:'.';}33%{content:'..';}66%{content:'...';}}"
      "@keyframes sp{to{transform:rotate(360deg)}}"
      "</style></head>"
    ));
    yield();

    // ── HTML body ────────────────────────────────────────────────────
    server->sendContent_P(PSTR(
      "<body><div class=\"card\">"
      "<div class=\"header\">"
      "<h2>Gas Leak Detector</h2>"
      "<button class=\"gear-btn\" id=\"gearBtn\" onclick=\"toggleSettings()\" title=\"Settings\">&#9881;</button>"
      "</div>"
      "<div class=\"body\">"
      // WiFi panel
      "<div class=\"panel active\" id=\"wifiPanel\">"
      "<div class=\"info\">Connect the device to your WiFi network to start sending sensor data to the cloud.</div>"
      "<div class=\"fg\"><label>WiFi Network</label>"
      "<div class=\"scan-row\">"
      "<select id=\"ssidSel\" onchange=\"document.getElementById('ssidIn').value=this.value\">"
      "<option value=\"\">Select a network</option>"
      "</select>"
      "<button class=\"scan-btn\" id=\"scanBtn\" onclick=\"scanWiFi()\" title=\"Scan\">"
      "<span id=\"scanIcon\">&#x27F3;</span>"
      "</button></div>"
      "<div class=\"scan-status\" id=\"scanStatus\"></div>"
      "<input type=\"text\" id=\"ssidIn\" placeholder=\"Or type SSID manually\">"
      "</div>"
      "<div class=\"fg\"><label>Password</label><input type=\"password\" id=\"pass\" placeholder=\"WiFi password\"></div>"
      "<div class=\"fg\"><label>API Key</label><input type=\"text\" id=\"apiKey\" placeholder=\"Your API key\"></div>"
      "<button class=\"btn btn-primary\" id=\"connectBtn\" onclick=\"save()\">Connect</button>"
      "<button class=\"btn btn-sec\" onclick=\"checkStatus()\">Check Status</button>"
      "<div class=\"msg\" id=\"msg\"></div>"
      "</div>"
    ));
    yield();

    // Settings panel
    server->sendContent_P(PSTR(
      "<div class=\"panel\" id=\"settingsPanel\">"
      "<div class=\"info\">API host without protocol prefix.<br>e.g.&nbsp; myapp.vercel.app</div>"
      "<div class=\"fg\"><label>API Host</label><input type=\"text\" id=\"apiUrl\" placeholder=\"myapp.vercel.app\"></div>"
      "<div class=\"fg\"><label>Send Interval (ms)</label>"
      "<input type=\"number\" id=\"sendInterval\" min=\"200\" max=\"5000\" step=\"100\" value=\"200\">"
      "<div class=\"hint\">500&nbsp;=&nbsp;~1.5&nbsp;req/s &nbsp;&bull;&nbsp; 800&nbsp;=&nbsp;~1&nbsp;req/s</div></div>"
      "<div class=\"fg\"><label>PPM Thresholds</label>"
      "<div class=\"grid2\">"
      "<div><label style=\"color:#ff9800;font-size:12px\">&#9888; Warning (ppm &ge;)</label>"
      "<input type=\"number\" id=\"ppmWarning\" min=\"10\" max=\"1990\" step=\"10\" value=\"300\"></div>"
      "<div><label style=\"color:#f44336;font-size:12px\">&#128293; Danger (ppm &ge;)</label>"
      "<input type=\"number\" id=\"ppmDanger\" min=\"10\" max=\"2000\" step=\"10\" value=\"800\"></div>"
      "</div>"
      "<div class=\"hint\">Normal &rarr; Warning &rarr; Danger as PPM increases</div></div>"
      "<div class=\"fg\"><label style=\"color:#ff5722;font-size:12px\">&#128276; Buzzer (ppm &ge;)</label>"
      "<input type=\"number\" id=\"ppmBuzzer\" min=\"10\" max=\"2000\" step=\"10\" value=\"300\"></div>"
      // Device Identity section (optional fields)
      "<div style=\"border-top:1px solid #2a2a2a;margin:20px 0 16px;padding-top:16px\">"
      "<p style=\"font-size:12px;color:#555;margin-bottom:12px;text-transform:uppercase;letter-spacing:.5px\">Device Identity (optional)</p>"
      "<div class=\"fg\"><label>Device ID</label>"
      "<input type=\"text\" id=\"deviceId\" maxlength=\"31\" value=\"ESP_GASLEAK_01\">"
      "<div class=\"hint\">Unique ID sent with every reading. Leave blank to use default.</div></div>"
      "<div class=\"fg\"><label>Device Name</label>"
      "<input type=\"text\" id=\"devName\" maxlength=\"63\" value=\"kitchen\"></div>"
      "<div class=\"fg\"><label>Location (GPS)</label>"
      "<div class=\"grid2\">"
      "<div><label style=\"font-size:11px;color:#666\">Latitude</label>"
      "<input type=\"number\" id=\"lat\" step=\"0.000001\" min=\"-90\" max=\"90\" placeholder=\"10.762622\"></div>"
      "<div><label style=\"font-size:11px;color:#666\">Longitude</label>"
      "<input type=\"number\" id=\"lng\" step=\"0.000001\" min=\"-180\" max=\"180\" placeholder=\"106.660172\"></div>"
      "</div>"
      "<div class=\"hint\">Same format as Google Maps / GPS. Leave blank if not needed.</div></div>"
      "</div>"
      "<button class=\"btn btn-primary\" onclick=\"saveSettings()\">Save Settings</button>"
      "<div class=\"msg\" id=\"msgS\"></div>"
      "</div>"
      "</div></div>"
    ));
    yield();

    // ── JavaScript ───────────────────────────────────────────────────
    server->sendContent_P(PSTR(
      "<script>"
      "var scanTimer,scanning=false,connectTimer=null,connectDeadline=0,connecting=false;"
      "function toggleSettings(){"
        "var wp=document.getElementById('wifiPanel');"
        "var sp=document.getElementById('settingsPanel');"
        "var gb=document.getElementById('gearBtn');"
        "var isSett=sp.classList.contains('active');"
        "wp.classList.toggle('active',isSett);"
        "sp.classList.toggle('active',!isSett);"
        "gb.classList.toggle('active',!isSett);"
      "}"
      "window.onload=function(){"
        "scanWiFi();"
        "fetch('/getsettings').then(r=>r.json()).then(d=>{"
          "if(d.apiUrl)document.getElementById('apiUrl').value=d.apiUrl;"
          "document.getElementById('sendInterval').value=d.sendInterval>0?d.sendInterval:200;"
          "document.getElementById('ppmWarning').value=d.ppmWarning>0?d.ppmWarning:300;"
          "document.getElementById('ppmDanger').value=d.ppmDanger>0?d.ppmDanger:800;"
          "document.getElementById('ppmBuzzer').value=d.ppmBuzzer>0?d.ppmBuzzer:300;"
          "document.getElementById('deviceId').value=d.deviceId&&d.deviceId.length?d.deviceId:'ESP_GASLEAK_01';"
          "document.getElementById('devName').value=d.name&&d.name.length?d.name:'kitchen';"
          "if(d.lat&&d.lat!==0)document.getElementById('lat').value=d.lat;"
          "if(d.lng&&d.lng!==0)document.getElementById('lng').value=d.lng;"
        "}).catch(()=>{});"
      "};"
    ));
    yield();

    server->sendContent_P(PSTR(
      "function showMsg(id,html,cls){"
        "var el=document.getElementById(id);"
        "el.style.display='block';el.innerHTML=html;"
        "el.className='msg msg-'+cls;"
      "}"
      "function setConnecting(on){"
        "connecting=on;"
        "var btn=document.getElementById('connectBtn');"
        "btn.disabled=on;"
        "btn.textContent=on?'Connecting...':'Connect';"
      "}"
      "function scanWiFi(){"
        "if(scanning)return;"
        "scanning=true;"
        "var btn=document.getElementById('scanBtn');"
        "var icon=document.getElementById('scanIcon');"
        "var st=document.getElementById('scanStatus');"
        "btn.disabled=true;"
        "icon.style.animation='sp .8s linear infinite';"
        "st.textContent='Scanning...';"
        "fetch('/scan').then(()=>{"
          "scanTimer=setInterval(checkScan,2000);"
        "}).catch(()=>{st.textContent='Scan failed';resetScan();});"
      "}"
      "function resetScan(){"
        "scanning=false;"
        "document.getElementById('scanBtn').disabled=false;"
        "document.getElementById('scanIcon').style.animation='';"
      "}"
    ));
    yield();

    server->sendContent_P(PSTR(
      "function checkScan(){"
        "fetch('/scanresult').then(r=>r.json()).then(d=>{"
          "if(d.status==='done'){"
            "clearInterval(scanTimer);"
            "var sel=document.getElementById('ssidSel');"
            "var nets=d.networks||[];"
            "sel.innerHTML='<option value=\"\">Select a network</option>';"
            "nets.forEach(n=>sel.innerHTML+='<option value=\"'+n+'\">'+n+'</option>');"
            "document.getElementById('scanStatus').textContent=nets.length?nets.length+' networks found':'';"
            "resetScan();"
          "}"
        "}).catch(()=>{});"
      "}"
      "function checkStatus(){"
        "showMsg('msg','Checking...','warn');"
        "fetch('/status').then(r=>r.json()).then(d=>{"
          "d.connected"
          "?showMsg('msg','\u2713 Connected \u00b7 '+d.ssid+' \u00b7 '+d.sta_ip,'ok')"
          ":showMsg('msg','Not connected','err');"
        "}).catch(()=>showMsg('msg','Request failed','err'));"
      "}"
      "function save(){"
        "if(connecting)return;"
        "var ssid=document.getElementById('ssidIn').value.trim()||document.getElementById('ssidSel').value;"
        "var pass=document.getElementById('pass').value;"
        "var apiKey=document.getElementById('apiKey').value.trim();"
        "if(!ssid||ssid==='Select a network'){showMsg('msg','Select or enter a WiFi network','err');return;}"
        "if(!apiKey){showMsg('msg','Enter API Key','err');return;}"
        "setConnecting(true);"
        "showMsg('msg','Connecting<span class=\"dots\"></span>','warn');"
        "fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({ssid:ssid,pass:pass,apiKey:apiKey})})"
        ".then(r=>r.text().then(t=>({ok:r.ok,text:t})))"
        ".then(r=>{"
          "if(r.ok){"
            "connectDeadline=Date.now()+20000;"
            "pollStatus();"
          "}else{"
            "setConnecting(false);"
            "showMsg('msg','Error: '+r.text,'err');"
          "}"
        "}).catch(()=>{setConnecting(false);showMsg('msg','Request failed','err');});"
      "}"
    ));
    yield();

    server->sendContent_P(PSTR(
      "function pollStatus(){"
        "if(!connecting)return;"
        "fetch('/status').then(r=>r.json()).then(d=>{"
          "if(d.connected){"
            "setConnecting(false);"
            "showMsg('msg','\u2713 Connected \u00b7 '+d.ssid+' \u00b7 '+d.sta_ip,'ok');"
          "}else if(Date.now()<connectDeadline){"
            "connectTimer=setTimeout(pollStatus,1500);"
          "}else{"
            "setConnecting(false);"
            "showMsg('msg','\u2717 Wrong password or unreachable network','err');"
          "}"
        "}).catch(()=>{"
          "if(connecting&&Date.now()<connectDeadline)connectTimer=setTimeout(pollStatus,1500);"
          "else{setConnecting(false);showMsg('msg','\u2717 Wrong password or unreachable network','err');}"
        "});"
      "}"
      "function saveSettings(){"
        "var apiUrl=document.getElementById('apiUrl').value.trim();"
        "var si=parseInt(document.getElementById('sendInterval').value);"
        "var pw=parseInt(document.getElementById('ppmWarning').value);"
        "var pd=parseInt(document.getElementById('ppmDanger').value);"
        "var pb=parseInt(document.getElementById('ppmBuzzer').value);"
        "if(!apiUrl){showMsg('msgS','Enter API Host','err');return;}"
        "if(isNaN(si)||si<200){showMsg('msgS','Send interval min 200ms','err');return;}"
        "if(isNaN(pw)||pw<=0){showMsg('msgS','Enter Warning threshold','err');return;}"
        "if(pd<=pw){showMsg('msgS','Danger must be > Warning','err');return;}"
        "if(isNaN(pb)||pb<=0){showMsg('msgS','Enter Buzzer threshold','err');return;}"
        "if(apiUrl.indexOf('://')>0)apiUrl=apiUrl.split('://')[1];"
        // collect optional identity fields
        "var devId=document.getElementById('deviceId').value.trim();"
        "var devName=document.getElementById('devName').value.trim();"
        "var latV=parseFloat(document.getElementById('lat').value);"
        "var lngV=parseFloat(document.getElementById('lng').value);"
        "showMsg('msgS','Saving...','warn');"
        "fetch('/savesettings',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({apiUrl:apiUrl,sendInterval:si,ppmWarning:pw,ppmDanger:pd,ppmBuzzer:pb,"
            "deviceId:devId,name:devName,lat:isNaN(latV)?0:latV,lng:isNaN(lngV)?0:lngV})})"
        ".then(r=>r.text().then(t=>({ok:r.ok,text:t})))"
        ".then(r=>r.ok?showMsg('msgS','\u2713 Saved (reset ESP to use the new configuration)','ok'):showMsg('msgS','Error: '+r.text,'err'))"
        ".catch(()=>showMsg('msgS','Request failed','err'));"
      "}"
      "</script></body></html>"
    ));

    server->sendContent("");  // flush / end chunked transfer
  }


  String scanNetworks() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return "{\"status\":\"scanning\"}";
    if (n == WIFI_SCAN_FAILED)  { WiFi.scanNetworks(true); return "{\"status\":\"scanning\"}"; }
    String json = "{\"status\":\"done\",\"networks\":[";
    for (int i = 0; i < n && i < 20; i++) {
      if (i > 0) json += ",";
      json += "\"" + WiFi.SSID(i) + "\"";
    }
    WiFi.scanDelete();
    json += "]}";
    return json;
  }

  String saveConfig() {
    if (!server->hasArg("plain")) return "No data";
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server->arg("plain"))) return "Invalid JSON";

    String ssid   = doc["ssid"]   | "";
    String pass   = doc["pass"]   | "";
    String apiKey = doc["apiKey"] | "";
    ssid.trim(); pass.trim(); apiKey.trim();
    if (ssid.length() == 0)   return "Invalid SSID";
    if (apiKey.length() == 0) return "Invalid API Key";

    char savedUrl[128];
    strncpy(savedUrl, config.apiUrl, sizeof(savedUrl));
    DeviceParams savedParams = config.params;

    memset(&config, 0, sizeof(config));
    ssid.toCharArray(config.ssid,     sizeof(config.ssid));
    pass.toCharArray(config.password, sizeof(config.password));
    apiKey.toCharArray(config.apiKey, sizeof(config.apiKey));
    strncpy(config.apiUrl, savedUrl, sizeof(config.apiUrl));
    config.params = savedParams;
    config.valid  = true;

    char token[40];
    sprintf(token, "%08X-%04X-%04X-%04X-%012X",
      random(0xFFFFFFFF), random(0xFFFF), random(0xFFFF),
      random(0xFFFF), random(0xFFFFFFFF));
    strcpy(config.token, token);

    EEPROM.put(0, config);
    EEPROM.commit();

    // Trigger a connection attempt immediately; no reboot required.
    WiFi.disconnect();
    delay(100);
    WiFi.begin(config.ssid, config.password);
    isConnecting     = true;
    connectStartTime = millis();

    return "OK";
  }

  String saveSettings() {
    if (!server->hasArg("plain")) return "No data";
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, server->arg("plain"))) return "Invalid JSON";

    String apiUrl = doc["apiUrl"] | "";
    apiUrl.trim();
    if (apiUrl.length() == 0) return "Invalid API Host";

    int si = doc["sendInterval"] | 0;
    int pw = doc["ppmWarning"]   | 0;
    int pd = doc["ppmDanger"]    | 0;
    int pb = doc["ppmBuzzer"]    | 0;

    if (si < 200)  return "sendInterval min 200";
    if (pw <= 0)   return "Invalid ppmWarning";
    if (pd <= pw)  return "ppmDanger must be > ppmWarning";
    if (pb <= 0)   return "Invalid ppmBuzzer";

    apiUrl.toCharArray(config.apiUrl, sizeof(config.apiUrl));
    config.params = { si, pw, pd, pb };

    // Optional device identity fields (all may be empty).
    String devId = doc["deviceId"] | "";
    String name  = doc["name"]     | "";
    devId.trim(); name.trim();
    devId.toCharArray(config.deviceId, sizeof(config.deviceId));
    name.toCharArray(config.name,     sizeof(config.name));

    // 0.0 is the sentinel value for "not configured".
    config.latitude  = doc["lat"] | 0.0;
    config.longitude = doc["lng"] | 0.0;

    EEPROM.put(0, config);
    EEPROM.commit();
    return "OK";
  }

public:
  WiFiManager()
    : apStarted(false), lastConnectionCheck(0), connectStartTime(0),
      isConnecting(false), apIP(192, 168, 4, 1), netMask(255, 255, 255, 0),
      server(nullptr), dns(nullptr) {
    memset(&config, 0, sizeof(config));
  }

  ~WiFiManager() {
    if (server) { server->stop(); delete server; }
    stopDNS();
  }

  
  // Returns true when every byte in s is printable ASCII (0x20–0x7E).
  // Used to detect EEPROM garbage after a struct layout change.
  bool isValidStr(const char* s, int maxLen) {
    if (s[0] == '\0' || s[0] == (char)0xFF) return true; // empty = ok
    for (int i = 0; i < maxLen && s[i] != '\0'; i++) {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c > 0x7E) return false; // non-printable = garbage
    }
    return true;
  }

  void init() {
    EEPROM.get(0, config);
    if (config.valid != true || config.ssid[0] == (char)0xFF) {
      memset(&config, 0, sizeof(config));
      config.valid = false;
    } else {
      // Validate optional identity fields; clear any EEPROM garbage.
      if (!isValidStr(config.deviceId, sizeof(config.deviceId)))
        memset(config.deviceId, 0, sizeof(config.deviceId));
      if (!isValidStr(config.name, sizeof(config.name)))
        memset(config.name, 0, sizeof(config.name));
      if (!isValidStr(config.apiUrl, sizeof(config.apiUrl)))
        memset(config.apiUrl, 0, sizeof(config.apiUrl));
      // Reset out-of-range GPS coordinates to 0.0 (= not set).
      if (isnan(config.latitude)  || config.latitude  < -90  || config.latitude  > 90)
        config.latitude  = 0.0;
      if (isnan(config.longitude) || config.longitude < -180 || config.longitude > 180)
        config.longitude = 0.0;
    }
    apName = buildAPName();
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_OFF); delay(100);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, netMask);
    WiFi.softAP(apName.c_str(), AP_PASSWORD);
    delay(500);
    startDNS();
    setupWebServer();
    apStarted = true;
  }

  DeviceParams getResolvedParams() { return resolveParams(config.params); }
  bool hasConfig() { return config.valid && strlen(config.ssid) > 0; }

  void handleClient() {
    if (dns)    dns->processNextRequest();
    if (server) server->handleClient();
    yield();
  }

  bool connectToWiFi() {
    if (!hasConfig()) return false;
    WiFi.begin(config.ssid, config.password);
    isConnecting     = true;
    connectStartTime = millis();
    return false;
  }

  bool checkConnection() {
    if (!isConnecting) return false;
    if (WiFi.status() == WL_CONNECTED) { isConnecting = false; return true; }
    if (millis() - connectStartTime > 30000) {
      isConnecting = false;
      WiFi.disconnect();
      return false;
    }
    return false;
  }

  bool isConnected() {
    if (millis() - lastConnectionCheck >= CONNECTION_CHECK_INTERVAL) {
      lastConnectionCheck = millis();
      if (WiFi.status() != WL_CONNECTED && hasConfig() && !isConnecting)
        WiFi.reconnect();
    }
    return WiFi.status() == WL_CONNECTED;
  }

  bool isConnectingNow() { return isConnecting; }
  DeviceConfig getConfig() { return config; }
  String       getAPName() { return apName; }
};

#endif
