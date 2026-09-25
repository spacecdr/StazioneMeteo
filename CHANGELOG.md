# Modifiche

## Versione pubblica dedicata

- Documentazione in italiano: scopo, hardware, cablaggio, uso, Wi-Fi, OTA e limiti.
- Illustrazione funzionale e schermate dell'HTML effettivamente incluso nel firmware.
- Rimosse credenziali Wi-Fi incorporate: l'avvio usa SSID e password salvati in NVS.
- Rimossa la stampa della password Wi-Fi sulla seriale; chiusa la sessione Preferences dopo il salvataggio.
- Sostituiti i valori del login dimostrativo con `admin` / `admin`; documentata l'assenza di autenticazione server.
- Rimossa la porta USB specifica della macchina di sviluppo.
- Fissate le versioni delle dipendenze, con DHT 1.4.6 disponibile nel registry.
- Esclusi cache, metadati macOS e configurazione locale dell'editor.
- Aggiunta compilazione automatica GitHub Actions.

La logica originale di acquisizione, menu, audio e OTA è mantenuta, salvo gli interventi elencati. Le limitazioni note sono riportate nel README.
