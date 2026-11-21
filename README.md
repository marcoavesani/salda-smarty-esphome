# VMC Salda Smarty → ESPHome → Home Assistant

> **English summary.** ESPHome configuration and custom components to control a **Salda Smarty** heat-recovery ventilation unit (MCB/miniMCB controller, Modbus RTU over RS485) from Home Assistant.
>
> | Component | What it does |
> |---|---|
> | `salda_sniffer` | Passively listens to the bus between the Salda S-Touch panel and the unit. It measures polling, bus gaps and latency, and decodes register values. |
> | `salda_proxy` | Sits between the panel and the unit with two RS485 ports, so the panel and Home Assistant can work together. |
> | `salda_common` | Shared Modbus RTU frame parser. |
>
> - Ready-made packages expose temperatures, fans, mode, set points, boost, filters and alarms.
> - Tested on PC with simulated traffic. Hardware: M5Stack Atom Lite + Atomic RS485 Base; XIAO ESP32-C6 variants included.
> - Documentation and entity names are in Italian. MIT licence.

Controllo completo di una **Salda Smarty 2X** (scheda **miniMCB**, valido anche per 3X/4X) da Home Assistant tramite un **M5Stack Atom + Atomic RS485 Base** con ESPHome, in Modbus RTU sulla stessa porta usata dal pannello **S-Touch**.

**Hardware in uso:** M5Stack **AtomS3 Lite** (ESP32-S3) + **Atomic RS485 Base**, cioè una sola interfaccia RS485. Le varianti per XIAO ESP32-C6 e quelle con due porte (proxy) restano come alternative.

**Configurazione consigliata:** `salda-smarty-bus.yaml`, il **bus condiviso**.

- L'ESP sta in parallelo all'S-Touch, che resta al suo posto e continua a funzionare, con una sola interfaccia RS485.
- I dati che il pannello legge già arrivano ascoltando il bus.
- Il resto, e i comandi, viaggiano nelle pause del pannello. Le pause sono state misurate sul campo con `salda-smarty-sniffer.yaml`.

Le alternative sono l'ESP al posto del pannello (`salda-smarty.yaml`) e il proxy a due porte.

Non esisteva nulla di simile: l'integrazione ufficiale HA [`smarty`](https://www.home-assistant.io/integrations/smarty/) funziona solo via Modbus TCP con il gateway Salda MB-GATEWAY. Espone poche entità e ha un bug noto: le temperature sotto zero vengono lette come ~6551 °C ([home-assistant/core#160413](https://github.com/home-assistant/core/issues/160413)).

## Uso dei componenti da GitHub

Non serve copiare nulla nella cartella ESPHome, per esempio quella dell'add-on di Home Assistant: ESPHome scarica componenti e pacchetti da GitHub. **Punta sempre a un tag** (`@v0.3.0`), non a `main`, così una modifica al repository non cambia il firmware alla compilazione successiva.

### Misura del bus

Esempio per AtomS3 Lite + Atomic RS485 Base, in un file tuo nella cartella ESPHome. Con il vecchio Atom Lite (ESP32) usa `board: m5stack-atom`, `bus_rx: GPIO22`, `bus_tx: GPIO19`.

```yaml
substitutions:
  bus_rx: GPIO5         # AtomS3 (Lite) + Atomic RS485 Base
  bus_tx: GPIO6
  modbus_baud: "19200"
  modbus_parity: EVEN

esphome:
  name: salda-smarty-sniffer
  friendly_name: VMC Salda Sniffer

esp32:
  board: m5stack-atoms3
  framework:
    type: esp-idf

logger:
api:
  encryption:
    key: !secret api_encryption_key
ota:
  - platform: esphome
    password: !secret ota_password
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source: github://marcoavesani/salda-smarty-esphome@v0.3.0
    components: [salda_sniffer, salda_common]

packages:
  sniffer: github://marcoavesani/salda-smarty-esphome/packages/sniffer.yaml@v0.3.0
```

### Controllo completo (ESP unico master sulla linea)

Stessa intestazione dell'esempio precedente. Al posto di `external_components` e del pacchetto sniffer:

```yaml
time:
  - platform: homeassistant
    id: ha_time            # usato dal pulsante "Sincronizza orologio"

uart:
  id: uart_rs485
  tx_pin: GPIO19
  rx_pin: GPIO22
  baud_rate: 19200
  parity: EVEN

modbus:
  id: modbus_bus
  uart_id: uart_rs485

modbus_controller:
  - id: smarty             # i pacchetti cercano questo id
    modbus_id: modbus_bus
    address: 1
    update_interval: 10s

packages:
  smarty:
    url: https://github.com/marcoavesani/salda-smarty-esphome
    ref: v0.3.0
    files:
      - packages/monitor.yaml
      - packages/controls.yaml
      - packages/alarms.yaml
```

### Bus condiviso (consigliato)

Stessa intestazione dell'esempio "Misura del bus", con `name: salda-smarty`. Al posto di `external_components` e del pacchetto sniffer:

```yaml
time:
  - platform: homeassistant
    id: ha_time

external_components:
  - source: github://marcoavesani/salda-smarty-esphome@v0.4.0
    components: [salda_bus, salda_common]

uart:
  id: uart_bus
  rx_pin: GPIO5            # AtomS3 + Atomic RS485 Base
  tx_pin: GPIO6
  baud_rate: 19200
  parity: EVEN
  rx_buffer_size: 1024

salda_bus:
  id: bus
  bus_uart_id: uart_bus
  panel_connected:
    name: Pannello S-Touch attivo

modbus:
  id: modbus_bus
  uart_id: bus             # la UART virtuale di salda_bus
  send_wait_time: 5000ms

modbus_controller:
  - id: smarty
    modbus_id: modbus_bus
    address: 1
    update_interval: 15s

packages:
  smarty:
    url: https://github.com/marcoavesani/salda-smarty-esphome
    ref: v0.4.0
    files:
      - packages/monitor.yaml
      - packages/controls.yaml
      - packages/alarms.yaml
```

Le altre opzioni di `salda_bus` (entità diagnostiche, tempi) sono in `salda-smarty-bus.yaml`.

### Proxy

Per il proxy usa `components: [salda_proxy, salda_common]` e copia i blocchi `uart`, `salda_proxy` e `modbus` da `salda-smarty-proxy.yaml`.

I file `salda-smarty*.yaml` nella radice del repository sono esempi completi che usano i componenti locali (`path: components`).

## Due master sulla stessa linea: come funziona il bus condiviso

- **Una sola porta.** La miniMCB della Smarty ha **una sola linea RS485** utilizzabile, il connettore **X18**, in Modbus RTU.
- **Il pannello interroga di continuo.** L'S-Touch è **master** su quella linea (parametro di servizio F.17 = 0, default) e interroga l'unità senza sosta.
- **Due master ingenui collidono.** Un secondo master che trasmette quando gli pare, come il `modbus` standard di ESPHome collegato in parallelo, si scontra con il pannello e può prendere per sue le risposte destinate all'altro.

### Cosa è stato misurato

Con `salda-smarty-sniffer.yaml`, su una Smarty 2X con S-Touch:

| Grandezza | Valore |
|---|---|
| Ciclo di interrogazione | **23 richieste** in circa 2,2 s, circa 600 transazioni al minuto |
| Occupazione del bus | **8,6 %** |
| Pausa dopo ogni risposta | **50–100 ms** quasi sempre; minima **34 ms** |
| Latenza dell'unità | media **8 ms**, massima **15 ms** |

Le pause lunghe (decine di secondi) arrivano quando si naviga nei menu.

Cosa legge il pannello:

| Tipo | Indirizzi |
|---|---|
| Holding register | HR 1, 2, 3, 4, 6, 14, 21, 22, 23, 107, 202, 661, 662 |
| Coil | 1, 5, 8, 51, 53, 117, 125 |
| Input register | IR 18 (mandata), IR 30 (giorni filtri) |
| Discrete input | **tutti, da 1 a 339 in un solo blocco** |

Le scritture del pannello confermano la mappa dei registri:
- Il setpoint Comfort è `HR 2 = 215` per 21,5 °C.
- La modalità è `HR 1`, e il pannello scrive insieme la portata `HR 3` (20 % in Protezione edificio, 70 % in Comfort).
- Il boost si accende con `COIL 5 = 1` e si spegne con `COIL 5 = 0`.

### Cosa fa `salda_bus`

- **Ascolta tutto il traffico.** Ogni valore che passa finisce in una cache con il suo istante di aggiornamento.
- **Risponde a `modbus_controller` dalla cache** quando tutti i registri richiesti hanno meno di `cache_max_age` (10 s). In questo caso non trasmette niente.
- **Legge il resto in blocchi di al massimo 4 registri.** Ogni blocco parte **subito dopo che l'unità ha risposto al pannello**, che a quel punto aspetta almeno 34 ms prima della richiesta successiva. Una nostra transazione è stimata al massimo 30 ms, contando la latenza peggiore. Le scritture seguono la stessa regola.
- **Accetta una risposta solo se corrisponde alla sua richiesta.** Indirizzo, function code e quantità devono tornare. Se la risposta manca, ritrasmette nello slot successivo.
- **Con il pannello fermo** da più di 600 ms (menu, pannello staccato) il bus è libero e trasmette subito.
- **Eco del transceiver:** se il transceiver rimanda indietro ciò che l'ESP trasmette, la riconosce da sola e la scarta.

### Verifica in simulazione (`tests/test_bus.cpp`)

La simulazione modella il bus a livello di byte, con collisioni vere, sul traffico misurato. Il pannello, `modbus_controller` con le letture e scritture dei pacchetti e l'unità girano per 2 minuti per ogni scenario.

| Scenario | Risultato |
|---|---|
| Traffico come misurato | **0 collisioni**, nessun timeout, nessun dato sbagliato; 252 richieste HA su 302 servite dalla cache |
| Con eco del transceiver | Come sopra |
| Loop dell'ESP lento | Come sopra |
| Pannello assente | Come sopra |
| Pause del pannello strette (20–40 ms, peggiori del reale) | Qualche collisione, risolta con ritrasmissioni; **nessun dato sbagliato** né al pannello né a HA |

### Alternative

| Opzione | File | Cosa serve | Pro / contro |
|---|---|---|---|
| **Bus condiviso** (consigliata) | `salda-smarty-bus.yaml` | Una sola interfaccia in parallelo all'S-Touch | Pannello e HA insieme, nessun cablaggio da tagliare. In teoria, con pause molto più strette di quelle misurate, sono possibili rare collisioni, che vengono ritrasmesse |
| **ESP al posto dell'S-Touch** | `salda-smarty.yaml` | Stacchi l'S-Touch e colleghi l'Atom alla X18 | Il più semplice, ma perdi il pannello a muro |
| **Proxy** | `salda-smarty-proxy.yaml`, `salda-smarty-proxy-xiao-c6.yaml` | Due porte RS485 inserite nel cavo | Zero collisioni, ma serve una seconda interfaccia. Se l'ESP è spento, il pannello resta senza comunicazione ("noC") |

Tutte le varianti espongono le stesse entità in Home Assistant.

## Cablaggio (opzione A)

### Connettore X18 sulla miniMCB (RJ45, zona "F")

Fonti: manuale miniMCB v2024.1 e manuale SMARTY X v2026.1.

| Pin | Segnale |
|---|---|
| 1, 2, 3 | non usati (RS422 Z/Y sulle schede vecchie) |
| 4 | RS485 **A** |
| 5 | RS485 **B** |
| 6 | GND (segnale) |
| 7 | **+24 V DC** |
| 8 | GND |

- **Interruttore S2 pos. 1:** terminazione 120 Ω. Sulle schede vecchie S2 ha 6 posizioni: lì la pos. 6 disattiva l'isolamento galvanico (ON per un pannello remoto).
- **Lato S-Touch:** il cavo arriva al morsetto a vite: 1–2 = 24 V, 3 = GND, 4 = B, 5 = A, 6 = GND. Puoi collegare l'Atom agli stessi fili, lato pannello, dopo aver scollegato l'S-Touch.
- **Polarità A/B:** è incoerente tra i vari documenti Salda (A = D+ oppure A−). Se l'ESP non riceve risposte, **inverti A e B**: non si rompe nulla.

### Atom + Atomic RS485 Base

- **Bus:** A → A, B → B, GND → GND, sul morsetto a 4 poli della base. Controlla la serigrafia della base per l'ordine dei poli.
- **Alimentazione:** sul pin 7 della X18 ci sono 24 V.
  - Secondo la [documentazione M5Stack](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base), la base accetta **6–24 V** e alimenta l'Atom con un convertitore DC-DC integrato (AOZ1282).
  - Si può quindi alimentare dalla linea 24 V della Smarty. Siamo però al limite alto: **misura prima** la tensione reale tra i pin 7 e 8 della X18 (o tra i morsetti 1 e 3 dell'S-Touch).
  - In alternativa alimenta l'Atom dalla USB-C.
- **Pin UART:** Atom Lite/Matrix TX = GPIO19, RX = GPIO22 (default nel YAML). Per AtomS3: TX = GPIO6, RX = GPIO5. Si cambiano in `substitutions`.
- **Direzione TX/RX:** la base ha solo TX e RX, senza pin di direzione, quindi commuta da sola: niente `flow_control_pin`. Se usi un MAX485 con DE/RE, aggiungilo nel blocco `modbus:`.
- **Terminazione:** la base **non ha** la resistenza da 120 Ω ([tutorial M5Stack](https://docs.m5stack.com/en/arduino/projects/atomic/atomic_rs485_232_base)). Su tratti corti non serve.

## Misura del bus (sniffer, solo ascolto)

`salda-smarty-sniffer.yaml`: AtomS3 (Lite) + Atomic RS485 Base, collegata **in parallelo** ai fili A/B/GND dell'S-Touch. L'ESP **non trasmette mai**, quindi non può disturbare né il pannello né l'unità.

Alternativa: `salda-smarty-sniffer-xiao-c6.yaml`, con XIAO ESP32-C6 e una RS485 Breakout Board Seeed. Le due varianti condividono `packages/sniffer.yaml`.

Serve a capire se un secondo master può stare sulla stessa linea del pannello, trasmettendo solo nelle pause, senza proxy.

### Cosa misura

- **Ogni quanto l'S-Touch interroga l'unità.** Per ogni tipo di richiesta (function code, registri) riporta numero, intervallo medio, minimo e massimo, risposte mancate ed eccezioni.
- **Pause sul bus:** il tempo tra la fine di una risposta e la richiesta successiva, con minimo, media, massimo e istogramma. È il numero chiave: una nostra transazione dura circa 15–40 ms.
- **Latenza dell'unità** (fine richiesta → inizio risposta) e **occupazione del bus** in %.
- **Valori decodificati:** ogni registro letto o scritto dal pannello, utile anche per verificare l'indirizzamento della tabella. Le scritture del pannello vengono loggate subito, per esempio "Scrittura vista: FC06 HR 2 = 215". Così, cambiando un'impostazione dall'S-Touch, si vede quale registro tocca.

### Dove vedere i risultati

- **Nei log, ogni minuto:** riepilogo, istogramma delle pause, elenco delle richieste.
- **In Home Assistant:**
  - entità diagnostiche: transazioni/min, pausa minima e media, latenza max, occupazione, errori;
  - pulsanti: **Elenco richieste**, **Valori registri**, **Azzera statistiche**.
- **Senza Home Assistant:** il pulsante dell'Atom (GPIO41 sull'AtomS3, GPIO39 sull'Atom Lite) stampa subito nel log l'elenco delle richieste.

### Collegamento (AtomS3 + Atomic RS485 Base)

```
 S-Touch morsetti:  5 = A   4 = B   3 = GND   1-2 = +24 V
                      │       │       │          │ (opzionale: alimentazione)
 Atomic RS485 Base:   A       B      GND      VIN 6–24 V
```

- **Bus:** A, B e GND della base in parallelo a quelli dell'S-Touch. In alternativa collegati sulla X18 (4 = A, 5 = B, 6 = GND).
- **Alimentazione:** dalla USB-C dell'Atom, oppure dai 24 V del cavo del pannello sul polo di alimentazione della base. La base accetta 6–24 V: misura prima la tensione.
- **Ricezione garantita:** la base non ha resistenza di terminazione, quindi in parallelo non altera la linea. Il pin TX (GPIO6 sull'AtomS3, GPIO19 sull'Atom Lite) resta a riposo e la base resta in ricezione.
- **Errori di ricezione:** se lo sniffer vede solo errori, prova `modbus_parity: NONE`, poi a scambiare A e B.

### Collegamento (variante XIAO ESP32-C6 + scheda Seeed)

- **Bus:** come sopra, con l'interruttore **120R su NC**.
- **Alimentazione:** XIAO dalla USB-C. Nessun collegamento ai 24 V: la scheda Seeed accetta solo 5 o 12 V.
- **Pin TX e DE:** nel YAML D4 (TX) resta a riposo alto e D2 (DE) è forzato basso. Così il transceiver resta in ricezione, sia con la direzione automatica (V1.2) sia con quella software (V1.1).

### Come leggere i numeri

- **Pause minime sempre sopra ~60–80 ms:** un master che "ascolta prima di parlare" ha spazio per inserirsi, e il proxy non serve.
- **Pause di pochi ms (pannello che interroga a raffica):** serve il proxy.
- **Elenco delle richieste:** dice anche quali dati si possono avere **gratis**, solo ascoltando quello che il pannello legge già.

## Proxy (opzione B)

```
 S-Touch ──A/B──▶ [modulo RS485 su Grove]                    
                        Atom Lite (componente salda_proxy)   
 Smarty X18 ◀─A/B── [Atomic RS485 Base]                      
 +24 V / GND: passanti dalla Smarty all'S-Touch (non attraverso l'ESP)
```

### Come funziona

- **Verso l'S-Touch** l'ESP si comporta come l'unità: riceve le sue richieste, le inoltra alla Smarty e gli restituisce la risposta. Il pannello non si accorge di nulla e non va riconfigurato.
- **Verso la Smarty** l'ESP è l'unico master, quindi non ci sono collisioni.
- **Le richieste di Home Assistant** passano negli spazi tra una richiesta del pannello e la successiva.
  - Il pannello ha la precedenza.
  - Una richiesta HA che aspetta più di 300 ms passa davanti, una volta.
  - In simulazione la latenza aggiunta al pannello resta sotto i 35 ms con un'unità che risponde in 15 ms.
- **Protezioni**, verificate dai test in `tests/`:
  - CRC su ogni frame e risincronizzazione dopo byte sporchi.
  - Verifica che ogni risposta corrisponda alla richiesta: indirizzo, function code, quantità.
  - Risposte arrivate dopo che il pannello ha già ritrasmesso: scartate.
  - Dopo un timeout, pausa di guardia contro le risposte tardive.
  - Richieste HA già abbandonate da `modbus`: scartate.
- Il componente è anche una **UART virtuale**: nel YAML `modbus: uart_id: proxy`. I pacchetti `monitor`/`controls`/`alarms` sono quindi identici nelle due varianti.

### Hardware

- **Porta unità:** Atomic RS485 Base. TX GPIO19, RX GPIO22.
- **Porta pannello:** un secondo transceiver RS485 a 3,3 V sul connettore Grove dell'Atom (GPIO26 / GPIO32).
  - Per esempio M5Stack **Unit RS485** o **RS485-ISO**, oppure un qualsiasi modulo MAX3485/SP3485.
  - Con un modulo a pin DE/RE separati aggiungi `flow_control_pin` alla `uart_panel`.
  - Se "Pannello S-Touch collegato" resta spento, scambia `panel_tx` e `panel_rx`.
- **Alimentazione del pannello:**
  - L'S-Touch si alimenta dal cavo, ai morsetti 1–2 (24 V) e 3 (GND).
  - Quei fili devono **passare dritti** dalla Smarty al pannello. Solo A/B vanno interrotti e portati alle due porte dell'ESP.
  - Unisci anche i GND delle due porte RS485 al GND comune.
- **Terminazione:** ora ci sono due segmenti corti (unità↔ESP e ESP↔pannello). A 19200 baud la terminazione in genere non serve. In caso di errori attiva il 120 Ω (S2 pos. 1 sulla miniMCB, ponticello sul modulo).

### Proxy su XIAO ESP32-C6 (`salda-smarty-proxy-xiao-c6.yaml`)

Hardware: Seeed **XIAO ESP32-C6** e due **RS485 Breakout Board for XIAO** (TP8485E). Le schede V1.2 usano TX = D4, RX = D5, DE = D2 ([wiki Seeed](https://wiki.seeedstudio.com/XIAO-RS485-Expansion-Board/)). Di fabbrica la direzione è automatica e D2 non è usato.

**Scheda 1, lato Smarty.** La XIAO è inserita nel suo zoccolo e usa D4 (GPIO22) e D5 (GPIO23). Non serve nient'altro.

**Scheda 2, lato S-Touch.** È collegata con fili dai suoi zoccoli ai pin della XIAO che la scheda 1 non usa:

| Zoccolo scheda 2 | Pin XIAO |
|---|---|
| D4 (TX) | **D6** (GPIO16) |
| D5 (RX) | **D7** (GPIO17) |
| D2 (DE), solo per V1.1 o direzione software | **D8** (GPIO19); nel YAML togli il commento a `flow_control_pin: GPIO19` |
| 3V3 / GND | 3V3 / GND |

Il transceiver TP8485E è alimentato dal 3V3 della XIAO: lo dicono gli schemi V1.1 e V1.2. **Il 5V non va collegato tra le due schede.** Sulla scheda 2 alimenterebbe soltanto il circuito di uscita 5V e il LED.

- **Dove saldare:** con la XIAO inserita, D6, D7 e D8 si raggiungono sotto la scheda 1, sulle saldature dei rispettivi zoccoli (che la scheda 1 non usa), oppure sulle piazzole laterali della XIAO.
- **Alimentazione:** la scheda accetta **solo 5 V o 12 V**. Non collegarla direttamente ai 24 V della X18 (pin 7, "24VDC" nel manuale miniMCB).
  - Cosa c'è all'ingresso 12V, secondo gli schemi Seeed V1.1 e V1.2:
    - un diodo di protezione TVS **SMBJ15CA** (D5) tra 12V e GND;
    - un diodo Schottky B5819WS;
    - condensatori da 22 µF con tensione nominale **25 V**;
    - un buck **TI TPS54302** (12→5 V, ingresso 4,5–28 V).
  - Il buck da solo reggerebbe i 24 V. Il TVS però inizia a condurre intorno ai 16,7 V: con 24 V si comporterebbe quasi come un cortocircuito, si brucerebbe e caricherebbe la linea 24 V della Smarty (fusibile F1). Anche i condensatori da 25 V sarebbero al limite.
  - Serve quindi un convertitore 24→12 V da qualche centinaio di mA tra il pin 7 della X18 e il morsetto 12V, oppure l'alimentazione dalla USB-C.
  - Alimenta solo la scheda 1 e lascia vuoto il morsetto di alimentazione della scheda 2.
- **Massa:** il morsetto di alimentazione (12V / 5V / INT) **non ha GND**. La massa dell'alimentazione passa dal pin GND del morsetto RS485 (A / B / GND), che va comunque collegato al GND della Smarty.
- **Terminazione:** su tratti corti lascia gli interruttori "120R" su NC. Mettili su ON solo in caso di errori.
- **UART:** sulla C6 il logger usa la USB integrata (USB_SERIAL_JTAG), quindi le due UART hardware restano libere per le porte RS485.

### Entità diagnostiche del proxy

| Entità | Cosa indica |
|---|---|
| **Pannello S-Touch collegato** | Arriva traffico dal pannello |
| **Unità collegata** | L'unità risponde |
| **Proxy timeout unità** | Contatore dei timeout dell'unità |
| **Proxy errori** | Byte scartati, risposte tardive o obsolete |
| **Proxy latenza pannello max** | Latenza massima vista dal pannello nell'ultimo intervallo |
| Proxy richieste pannello / HA | Contatori, disabilitati di default |

Nei log (livello DEBUG) ogni 30 s c'è un riepilogo. A livello VERBOSE vedi ogni frame inoltrato.

### Parametri regolabili

Nel blocco `salda_proxy:`:

| Parametro | Default | Cosa fa |
|---|---|---|
| `response_timeout` | 500 ms | Attesa massima della risposta dell'unità |
| `local_expire` | 1200 ms | Età oltre la quale una richiesta HA viene scartata. **Deve restare sotto `send_wait_time`** del blocco `modbus` (1500 ms) |
| `frame_gap` | 20 ms | Silenzio che chiude un frame incompleto |

### Parametri seriali

- **Default MCB:** indirizzo **1**, **19200** baud, **8E1** (parità pari).
- Coincidono con i default dell'S-Touch: F.18 = 4 (19200), F.19 = 1 (E,8,1), F.21 = 1 (indirizzo). Il menu di servizio dell'S-Touch ha codice 022: controllali lì.
- Se c'è timeout, prova `modbus_parity: NONE`.

## Installazione

```bash
cp secrets.yaml.example secrets.yaml   # e compila i valori
esphome run salda-smarty-sniffer.yaml  # misura; poi salda-smarty.yaml o un proxy. Primo flash via USB, poi OTA
```

Su Windows lancia `esphome` da PowerShell o cmd: da Git Bash l'installazione di ESP-IDF fallisce ("MSys/Mingw is not supported").

### Test su PC

Il parser dei frame e la logica del proxy si testano senza hardware:

```bash
g++ -std=c++17 -Icomponents tests/test_frame.cpp -o test_frame && ./test_frame
g++ -std=c++17 -Icomponents tests/test_proxy.cpp -o test_proxy && ./test_proxy
g++ -std=c++17 -Icomponents tests/test_sniffer.cpp -o test_sniffer && ./test_sniffer
g++ -std=c++17 -O2 -Icomponents tests/test_bus.cpp -o test_bus && ./test_bus
```

`test_proxy` simula 2 minuti di un S-Touch che interroga con ritrasmissioni, di un'unità lenta o che a volte non risponde e di richieste continue da HA. La verifica principale: nessuno riceve mai una risposta destinata a un altro.

In Home Assistant il dispositivo compare da solo, in **Impostazioni → Dispositivi → ESPHome**.

### Verifica al primo avvio (log a livello INFO/DEBUG)

1. **Temperature IR 18–21:** devono essere plausibili, per esempio mandata 18–22 °C ed esterna come fuori.
   - Se sono sballate di un registro, l'unità usa l'indirizzamento "tabella − 1". In quel caso togli 1 a tutti gli `address:`.
   - Le implementazioni esistenti, testate su unità reali, usano gli indirizzi della tabella così come sono.
2. **"Modalità":** deve corrispondere a quella che vedevi sull'S-Touch.
3. **Solo dopo:** prova una scrittura innocua, per esempio "Setpoint Comfort" +0,5 °C e ritorno.

## Entità in Home Assistant

| Tipo | Entità |
|---|---|
| Comandi | **Modalità** (Standby / Protezione edificio / Economy / Comfort, cioè velocità 0–3), **Boost**, **Setpoint Comfort / Economy / Protezione edificio**, Durata boost, Inverno/Estate, Raffrescamento notturno, Protezione secchezza, Portata in base a UR, setpoint UR, **Portata aria** (%) |
| Pulsanti | Reset timer filtri, Reset allarmi, Sincronizza orologio (imposta data/ora dell'unità da HA), Riavvia ESP |
| Sensori | Stato sistema (testo), Modalità attuale, temperature T1–T4 e mandata richiesta, portata %, giri e m³/h dei due ventilatori, posizione bypass, efficienza recupero, giorni residui filtri, tempo residuo boost, stagione riscaldamento, raffrescamento notturno attivo |
| Opzionali (disabilitati) | Umidità/CO₂ mandata e ripresa (solo se i sensori sono installati), temperatura scambiatore |
| Allarmi | Allarme critico, Avviso attivo, numero allarmi, **Allarmi attivi** (testo con i codici come sull'S-Touch, es. "W14 Sostituire filtri"), più i singoli codici come entità diagnostiche |

Una card di esempio è in [homeassistant/dashboard-card.yaml](homeassistant/dashboard-card.yaml). La mappa completa dei registri usati, con le fonti, è in [docs/registri.md](docs/registri.md).

## Cose da non fare

- Non scrivere i coil **51** (salva EEPROM), **52** (riavvio), **200** (reset di fabbrica), né i registri di servizio **≥ 400**. Questi ultimi sono override delle uscite, tarature e parametri di comunicazione, e vanno sbloccati con password.
- Il YAML non li espone di proposito.

## Note e limiti noti

- **Boost:** il coil 5 a 1 avvia il boost per la "Durata boost", a 0 lo ferma. È quello che fa l'S-Touch, misurato sul bus, anche se la tabella Salda dice che 0 "non fa nulla".
- **IR 15 = 4:** viene mostrato come "Boost". Il significato del valore 4 non è documentato.
- **Firmware:** la mappa è stabile tra le tabelle MCB 1.21 (2018) e 1.30 (2026) per tutti i registri usati qui.
