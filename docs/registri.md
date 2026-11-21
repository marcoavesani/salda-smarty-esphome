# Mappa registri usati (Salda MCB / miniMCB)

## Indirizzamento

- Indirizzi = numero della tabella ufficiale, usato tale e quale come indirizzo PDU Modbus.
- È lo stesso indirizzamento di [pysmarty2](https://github.com/martinssipenko/pysmarty2) (integrazione HA ufficiale) e di [kanocz/modbus2mqtt](https://github.com/kanocz/modbus2mqtt), che lo usa su una Smart 4X V reale con un convertitore RS485 trasparente.
- Tutti i valori sono 16 bit con segno: la tabella usa −1 per "Auto" e ci sono temperature negative.

## Fonti

Tutte su [salda.lt/mcb](http://salda.lt/mcb/):

- MCB 1.29 Modbus table (2026-04-22)
- MCB 1.30 Modbus table (2026-06-30)
- MCB 1.21 Modbus table (2018-05-03)

I documenti Salda non sono inclusi nel repository: si scaricano dalla pagina [salda.lt/mcb](http://salda.lt/mcb/).
- Manuale miniMCB v2024.1
- Manuale tecnico Stouch v2019.1

## Holding register (FC03 lettura, FC06/FC16 scrittura)

| Ind. | Nome | Valori | Entità |
|---|---|---|---|
| 1 | Modalità di sistema | 0 Standby, 1 Protezione edificio (vel. 1), 2 Economy (vel. 2), 3 Comfort (vel. 3) | select Modalità |
| 2 | Setpoint Comfort | 160..300 ×0,1 °C (def. 210) | number |
| 3 | Portata aria in % | 0..100; il pannello la scrive insieme alla modalità | number |
| 4 | Setpoint Economy | 0 = risparmio, 160..300 ×0,1 °C (def. 180) | number |
| 6 | Setpoint Protezione edificio | 0 = risparmio, 160..300 ×0,1 °C (def. 165) | number |
| 14 | Durata boost | 0..18000 s (def. 600) | number (min) |
| 15 | Inverno/Estate | 0 Estate, 1 Inverno, 2 Da calendario, 3 Media 3 giorni (def. 3) | select |
| 21 / 22 | Setpoint UR estate / inverno | % (def. 70 / 50) | number |
| 24 | Soglia protezione secchezza | % (def. 30) | number |
| 100..106 | Data/ora (anno 0..99, mese, giorno, riservato, ore, minuti, secondi) | poi coil 2 = 1 | pulsante Sincronizza orologio |
| 107 | Tempo residuo boost | s | letto dal pannello |
| 202 | Reset allarmi | 1 = esegui | pulsante |

**Disponibili ma non esposti:**

| Ind. | Contenuto | Livello |
|---|---|---|
| 7–12 | Ingressi remoti | — |
| 16–20 | Calendario inverno | — |
| 25–32 | Orari e soglie raffrescamento notturno | — |
| 210–249 | Ferie | — |
| 250–309 | Programmazione settimanale: 10 eventi da 6 registri (stato, portata, temperatura, giorni, ora, minuto) | — |
| 330–390 | Storico allarmi | — |
| ≥ 400 | Override, tarature, comunicazione (672–677) | Adjuster/Service |

## Coil (FC01 lettura, FC05 scrittura)

| Ind. | Nome | Entità |
|---|---|---|
| 1 | Reset timer filtri | pulsante |
| 2 | Applica data/ora | usato dal pulsante orologio |
| 3 | Protezione secchezza | switch |
| 4 | Raffrescamento notturno | switch |
| 5 | Boost (portata intensiva) | switch |
| 8 | Portata in base a UR ripresa | switch |
| 51 / 52 / 200 | Salva EEPROM / Riavvio / Reset di fabbrica | **mai** |

## Input register (FC04)

| Ind. | Nome | Scala |
|---|---|---|
| 1 | Stato sistema | 0 Standby, 1 Prot. edificio, 2 Economy, 3 Comfort, 4 Emergenza, 5 Preparazione, 6 Apertura serrande, 7 Boost, 8 Raffr. riscaldatori, 9 Chiusura serrande, 10 Raffr. notturno, 11 Allarme critico, 12 Incendio, 13 Antigelo, 14 Cambio filtri, 15 UR bassa, 16 Sbrinamento DX, 17 Test serranda |
| 2 / 3 | Versione software / configurazione | — |
| 13 | Tempo residuo boost | s |
| 15 | Modalità corrente | 0..4 |
| 16 | Portata attuale | % |
| 17 | Temperatura mandata richiesta | ×0,1 °C |
| 18 / 19 / 20 / 21 | T1 mandata / T2 ripresa / T3 espulsione / T4 esterna | ×0,1 °C, con segno |
| 22 / 24 | UR mandata / ripresa | ×0,1 % |
| 23 / 25 | CO₂ mandata / ripresa | ppm |
| 28 | Numero allarmi attivi | — |
| 30 | Giorni residui filtri | d |
| 55 / 56 | Giri ventilatore mandata / ripresa | rpm |
| 58 / 62 | Portata mandata / ripresa | ×0,1 % |
| 60 / 64 | Portata mandata / ripresa | m³/h |
| 93 | Posizione bypass | ×0,1 % |
| 104 | Temperatura scambiatore | ×0,1 °C |
| 125 | Efficienza recupero | % |

## Discrete input (FC02)

| Ind. | Contenuto |
|---|---|
| 1..72 | Un bit per allarme/avviso. Il numero è il codice A.xx / W.xx dell'S-Touch |
| 188 | Almeno un allarme critico |
| 189 | Almeno un avviso |
| 207 | Stagione: 1 riscaldamento, 0 raffrescamento |
| 209 | Raffrescamento notturno attivo |
| 310 | Bypass chiuso |
