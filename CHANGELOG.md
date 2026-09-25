# Modifiche

## 2.5.1 · Pubblicazione della sorgente aggiornata

Sostituita la precedente importazione con il firmware 2.5.1 fornito in `Downloads/PIO-main/Stazione Meteo`.

- Dashboard responsive con replica LCD a punti, quattro tasti remoti e stato RGB.
- Orologio CET/CEST con cambio stagionale automatico e cinque stili delle cifre.
- Wi-Fi con tre reti, timeout, AP di recupero e captive portal.
- Storico di circa 24 ore, MQTT, soglie ambientali e metriche Prometheus.
- Player non bloccante, 37 melodie e BPM persistenti per brano.
- Cronometro, countdown e Nomi, Cose e Città con avvisi sonori.
- Backup/ripristino, diagnostica, log eventi, watchdog e OTA.
- README, API, catalogo melodie e schermate sostituiti con documentazione della versione corretta.
- Rimosse vecchie immagini e anteprime del login/upload standalone.

Queste funzionalità sono già presenti nel sorgente aggiornato fornito. L'unico adattamento funzionale per la pubblicazione è lasciare vuoti SSID e password bootstrap privati: il primo avvio senza reti usa l'AP. La cartella originale in Download non viene modificata. Le versioni delle dipendenze richieste sono conservate dal suo `platformio.ini`.

## Importazione iniziale superata

Il primo commit conteneva per errore una versione precedente, con il solo pannello OTA. Non descrive il firmware attuale: fare riferimento al ramo `main` e alla documentazione 2.5.1.
