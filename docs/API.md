# API e integrazioni · 2.5.1

Base URL: `http://stazionemeteo.local`, oppure l'IP LAN/AP della scheda. Hostname configurabile. API HTTP senza autenticazione né TLS: destinate a rete fidata. I percorsi seguenti sono quelli registrati in `setupWebServer()`.

## Lettura

| Metodo | Percorso | Risultato |
| --- | --- | --- |
| GET | `/` | Dashboard HTML completa |
| GET | `/api/state` | JSON con firmware, misure, stato LCD/pulsanti, Wi-Fi, timer, musica, MQTT, allarmi e diagnostica |
| GET | `/api/history` | `points`: campioni `{ts,t,h}` in ordine temporale, al massimo 288 |
| GET | `/api/log` | `entries`: eventi `{ms,text}`, al massimo 60 |
| GET | `/api/wifi/networks` | `running` e `networks`, risultati dell'ultima scansione |
| GET | `/api/config/backup` | Download JSON, schema 1, **incluse password Wi-Fi e MQTT** |
| GET | `/metrics` | Testo in formato Prometheus 0.0.4 |

`/api/state` restituisce `null` per temperatura/umidità non ancora disponibili. `lcd_matrix.cells` contiene 80 codici di carattere, in ordine di riga; `lcd_matrix.custom` contiene otto bitmap di otto righe per i caratteri CGRAM. Il browser usa questi dati per la replica LCD.

`buttons` contiene quattro oggetti con `id`, `title`, `hint` ed `enabled`. Le azioni dipendono dal menu corrente; non sono comandi globali per musica o timer. In stato OTA i tasti sono disabilitati.

Nello storico, `ts` è Unix time quando NTP è disponibile, altrimenti secondi dall'avvio. `t` è temperatura °C, `h` umidità %. Nel log `ms` è il tempo dall'avvio in millisecondi. Entrambi i buffer risiedono in RAM e ripartono dopo reset/sleep.

## Scrittura

Salvo ripristino e OTA, usare parametri query o `application/x-www-form-urlencoded` come fa la dashboard.

| Metodo | Percorso | Parametri / effetto |
| --- | --- | --- |
| POST | `/api/button` | `id=1..4`; esegue l'azione contestuale |
| POST | `/api/wifi/scan` | Nessun parametro; avvia scansione asincrona, HTTP 202; 409 se scansione/connessione già in corso |
| POST | `/api/wifi/connect` | `index` del risultato di scansione, `password`; HTTP 202, salvataggio solo dopo successo |
| POST | `/api/wifi/forget` | `index` nell'elenco delle reti salvate; rimuove dalla NVS |
| POST | `/api/mqtt` | `enabled=0/1`, `host`, `port`, `topic`, `user`, `pass`; password vuota mantiene quella esistente |
| POST | `/api/alarms` | `enabled=0/1`, `tmin`, `tmax`, `hmin`, `hmax`, `topic`; min < max, umidità fra 0 e 100 |
| POST | `/api/device` | `name`; normalizza il nome, salva e riavvia |
| POST | `/api/clockstyle` | `style=0..4`; applica e salva |
| POST | `/api/log/clear` | Cancella il buffer eventi |
| POST | `/api/config/restore` | Corpo `application/json`, backup schema 1; valida, applica e riavvia |
| POST | `/update` | Upload `multipart/form-data`, campo `update` con firmware `.bin` |

Stili: 0 Morbido, 1 Arrotondato, 2 Sottile, 3 Punti, 4 Tech. Il backup contiene `device`, `game`, `wifi`, `mqtt`, `alarms` e `music_bpm`; il campo `firmware` identifica la sorgente del backup. Non è una copia dell'intera flash. Le validazioni vengono eseguite prima di iniziare le scritture NVS, ma il ripristino non è una transazione resistente a interruzioni di alimentazione.

Il firmware risponde alle sonde captive portal `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`. In AP anche i percorsi sconosciuti vengono reindirizzati al portale.

## Esempi di sola lettura

```sh
curl http://stazionemeteo.local/api/state
curl http://stazionemeteo.local/api/history
curl http://stazionemeteo.local/metrics
```

## MQTT

Client PubSubClient su TCP, porta predefinita 1883. Configurabile dalla dashboard, disabilitato per impostazione iniziale. Tentativo di connessione ogni 10 secondi quando necessario. Non sono implementati TLS, subscription per comandi, discovery Home Assistant o Last Will.

**Misure**, topic predefinito `stazionemeteo/sensor`, ogni 30 secondi con misure disponibili, retained:

```json
{"temperature_c":23.0,"humidity_pct":48.0,"rssi":-52,"uptime_s":3600,"alarm":"ok"}
```

**Allarmi**, topic predefinito `stazionemeteo/alarm`, al cambio delle condizioni, retained:

```json
{"device":"stazionemeteo","state":"temperatura_alta","temperature_c":36.0,"humidity_pct":48.0}
```

Gli stati sono `ok`, `temperatura_bassa`, `temperatura_alta`, `umidita_bassa`, `umidita_alta`; più condizioni sono concatenate con virgole. Cambiare hostname non riscrive automaticamente i topic già salvati. L'implementazione usa pubblicazioni QoS 0, senza conferma applicativa: gli allarmi non sono un canale garantito.

## Prometheus

Esempio di configurazione del server Prometheus, da adattare alla propria rete:

```yaml
scrape_configs:
  - job_name: stazionemeteo
    metrics_path: /metrics
    scrape_interval: 30s
    static_configs:
      - targets: ['stazionemeteo.local:80']
```

Se il server non risolve mDNS, usare un IP stabile/riservato DHCP. Le metriche hanno prefisso `stazionemeteo_` e comprendono temperatura, umidità, connessioni, RSSI, AP, allarmi, uptime, heap, stile orologio, cronometro, countdown e stato del gioco. Temperatura e umidità vengono omesse se non ancora disponibili.
