#!/usr/bin/env python3
"""Extract WEB_PAGE unchanged; add a separate, read-only demo with synthetic data."""
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/main.cpp').read_text()
html = re.search(r'const char WEB_PAGE\[\] PROGMEM = R"HTML\((.*?)\)HTML";', source, re.S).group(1)
version = re.search(r'FW_VERSION = "([^"]+)"', source).group(1)
preview = ROOT / 'docs/preview'
preview.mkdir(parents=True, exist_ok=True)
(preview / 'index.html').write_text(html)
lines = ['25/09/2026 14:32:08', 'IP: 192.0.2.20', 'Umidita: 48.0 %', 'Temperatura: 23.0 C']
state = {
 'firmware': version, 'temperature_c': 23.0, 'humidity_pct': 48.0,
 'mode': 'home', 'home_page': 'weather', 'ap': False, 'scan_running': False,
 'device': {'name': 'stazionemeteo'}, 'clock': {'style': 0, 'name': 'Morbido'},
 'rgb': {'r': 0, 'g': 1, 'b': 0, 'meaning': 'connesso'},
 'timers': {'ncc_alarm': False, 'ncc_running': False, 'countdown_alarm': False,
            'countdown_running': False, 'stopwatch_running': False},
 'lcd_matrix': {'graphics': False, 'cells': [ord(c) for row in lines for c in row.ljust(20)[:20]], 'custom': [[0]*8 for _ in range(8)]},
 'buttons': [{'id':i, 'title': title, 'hint': hint, 'enabled': True} for i,title,hint in [
     (1,'Mostra orologio','Salta fra dati meteo e orologio HH:MM'),
     (2,'Menu musica',"Apre l’elenco delle melodie"),
     (3,'Menu timer','Apre Cronometro, Countdown e Nomi Cose e Città'),
     (4,'Deep sleep',"Mette l’ESP32 in deep sleep; il tasto rosso lo risveglia")]],
 'wifi': {'connected': True, 'ssid': 'Rete-Demo', 'ip': '192.0.2.20', 'rssi': -52},
 'saved': ['Rete-Demo'],
 'mqtt': {'enabled': True, 'connected': True, 'host': '192.0.2.10', 'port': 1883, 'topic': 'stazionemeteo/sensor', 'user': ''},
 'alarms': {'enabled': True, 'active': False, 'summary': 'ok', 'temp_min': 5, 'temp_max': 35, 'humidity_min': 20, 'humidity_max': 80, 'topic': 'stazionemeteo/alarm'},
 'diag': {'uptime_s': 86400, 'free_heap': 142000, 'min_free_heap': 128000, 'reset_reason': 'power_on', 'wake_reason': 'none', 'ap_clients': 0, 'sensor_age_s': 1, 'watchdog': True}
}
fixtures = {
 '/api/state': state,
 '/api/history': {'points': [{'ts': 1790245928 + i*300, 't': round(22+math.sin(i/30)*1.5,2), 'h': round(48+math.sin(i/45)*5,2)} for i in range(288)]},
 '/api/log': {'entries': [{'ms': 1000, 'text': 'DEMO: avvio firmware '+version}, {'ms': 5000, 'text': 'DEMO: Wi-Fi connesso'}, {'ms': 10000, 'text': 'DEMO: MQTT connesso'}]},
 '/api/wifi/networks': {'running': False, 'networks': [{'ssid': 'Rete-Demo', 'rssi': -52, 'open': False}]}
}
injection = '<script>\nconst demoFixtures = '+json.dumps(fixtures, ensure_ascii=False)+';\n'+r'''
window.fetch = async (url, options={}) => {
 const path = new URL(url, location.href).pathname;
 if ((options.method || 'GET') !== 'GET' || !(path in demoFixtures))
  return new Response('Anteprima dimostrativa: comandi disabilitati', {status: 409});
 return new Response(JSON.stringify(demoFixtures[path]), {headers: {'Content-Type':'application/json'}});
};
window.XMLHttpRequest = class { open() {} send() { this.responseText='Anteprima: nessun firmware inviato'; if(this.onload)this.onload(); } };
window.confirm = () => false;
window.addEventListener('DOMContentLoaded', () => {
 const notice=document.createElement('div');
 notice.textContent='ANTEPRIMA · dati dimostrativi · nessun dispositivo collegato';
 notice.style.cssText='padding:8px 16px;text-align:center;background:#18394b;color:#d5f7ff;font:13px system-ui';
 document.querySelector('header').prepend(notice);
 document.querySelectorAll('a').forEach(a=>a.addEventListener('click',e=>e.preventDefault()));
 document.querySelectorAll('button[onclick="backupConfig()"],button[onclick="restoreConfig()"]')
   .forEach(b=>{b.disabled=true;});

});
</script>
'''
(preview / 'demo.html').write_text(html.replace('<script>', injection+'<script>',1))
print('Exported unmodified WEB_PAGE and isolated demo, firmware',version)
