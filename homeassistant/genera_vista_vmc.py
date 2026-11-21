"""Genera homeassistant/vmc-view.yaml (vista "VMC" per una dashboard a sezioni).

Lo schema dello scambiatore (vmc-schema.svg) viene incorporato come data URI,
così non serve caricare file in /config/www. Gli entity_id usano il prefisso
del dispositivo ESPHome "VMC Salda Smarty" nell'area "Quadro elettrico":
se il tuo è diverso, cambia PREFIX.

Uso: python homeassistant/genera_vista_vmc.py
"""

import json
from pathlib import Path
import urllib.parse

import yaml

HERE = Path(__file__).parent
PREFIX = "quadro_elettrico_vmc_salda_smarty_"


def e(domain: str, name: str) -> str:
    return f"{domain}.{PREFIX}{name}"


def svg_data_uri() -> str:
    svg = (HERE / "vmc-schema.svg").read_text(encoding="utf-8")
    lines = [ln.strip() for ln in svg.splitlines() if not ln.strip().startswith("<!--")]
    return "data:image/svg+xml," + urllib.parse.quote(" ".join(lines), safe=" =:/\"'(),;.-")


def tile(entity, name=None, icon=None, full=False, features=None, inline=False, **extra):
    c = {"type": "tile", "entity": entity}
    if name:
        c["name"] = name
    if icon:
        c["icon"] = icon
    if full:
        c["grid_options"] = {"columns": "full"}
    if features:
        c["features"] = features
        if inline:
            c["features_position"] = "inline"
    c.update(extra)
    return c


def press(entity, name, icon, confirm=None):
    action = {"action": "perform-action", "perform_action": "button.press", "target": {"entity_id": entity}}
    if confirm:
        action["confirmation"] = {"text": confirm}
    return tile(entity, name, icon, hide_state=True, tap_action=action, color="primary")


NUMBER_BUTTONS = [{"type": "numeric-input", "style": "buttons"}]
TOGGLE = [{"type": "toggle"}]
SELECT = [{"type": "select-options"}]


def label(entity, left, top, size, anchor="left", color="var(--primary-text-color)", weight=600, prefix=None):
    transform = {"left": "translate(0, -50%)", "center": "translate(-50%, -50%)", "right": "translate(-100%, -50%)"}
    el = {
        "type": "state-label",
        "entity": entity,
        "style": {
            "left": left,
            "top": top,
            "transform": transform[anchor],
            "font-size": size,
            "font-weight": weight,
            "color": color,
            "white-space": "nowrap",
        },
    }
    if prefix:
        el["prefix"] = prefix
    return el


T_EST = e("sensor", "temperatura_esterna_t4")
T_MAN = e("sensor", "temperatura_mandata_t1")
T_RIP = e("sensor", "temperatura_ripresa_t2")
T_ESP = e("sensor", "temperatura_espulsione_t3")
MUTED = "var(--secondary-text-color)"

schema = {
    "type": "picture-elements",
    "image": svg_data_uri(),
    "grid_options": {"columns": "full"},
    "elements": [
        label(T_EST, "4%", "25%", "30px"),
        label(T_RIP, "96%", "25%", "30px", anchor="right"),
        label(T_MAN, "96%", "84%", "30px", anchor="right"),
        # la sonda di espulsione è opzionale: senza, l'unità riporta 0
        {
            "type": "conditional",
            "conditions": [{"entity": T_ESP, "state_not": "0.0"}],
            "elements": [label(T_ESP, "4%", "84%", "30px")],
        },
        label(e("sensor", "modalita_attuale"), "50%", "27%", "16px", anchor="center", color=MUTED, weight=500),
        label(e("sensor", "posizione_bypass"), "50%", "84%", "15px", anchor="center", color=MUTED, weight=500,
              prefix="Bypass "),
        label(e("sensor", "ventilatore_ripresa_giri"), "33%", "84%", "14px", anchor="center", color=MUTED,
              weight=500),
        label(e("sensor", "ventilatore_mandata_giri"), "67%", "84%", "14px", anchor="center", color=MUTED,
              weight=500),
    ],
}

grafico = {
    "type": "custom:mini-graph-card",
    "name": "Ultime 24 ore",
    "icon": "mdi:chart-line",
    "hours_to_show": 24,
    "points_per_hour": 4,
    "line_width": 2,
    "height": 130,
    "show": {"fill": False, "points": False, "legend": True, "labels": False},
    "grid_options": {"columns": "full"},
    "entities": [
        {"entity": T_EST, "name": "Esterno", "color": "#3ccf91"},
        {"entity": T_MAN, "name": "Mandata", "color": "#4aa3ff"},
        {"entity": T_RIP, "name": "Ripresa", "color": "#f5c542"},
    ],
}

view = {
    "title": "VMC",
    "path": "vmc",
    "icon": "mdi:hvac",
    "type": "sections",
    "max_columns": 3,
    "sections": [
        {
            "type": "grid",
            "cards": [
                {
                    "type": "heading",
                    "heading": "Stato",
                    "icon": "mdi:hvac",
                    "badges": [
                        {"type": "entity", "entity": e("binary_sensor", "pannello_s_touch_attivo"),
                         "show_state": True, "show_icon": True, "color": "green"},
                    ],
                },
                tile(e("sensor", "stato_sistema"), "Stato", "mdi:state-machine", full=True, vertical=False),
                tile(e("select", "modalita"), "Modalità", "mdi:fan", full=True, features=SELECT),
                tile(e("switch", "boost"), "Boost", "mdi:fan-plus", full=True, features=TOGGLE, inline=True),
                tile(e("sensor", "boost_tempo_residuo"), "Boost residuo", "mdi:timer-sand"),
                tile(e("sensor", "ventilatore_mandata_portata"), "Ventilatori", "mdi:fan"),
                tile(e("number", "portata_aria"), "Portata", "mdi:fan-chevron-up", full=True,
                     features=NUMBER_BUTTONS, inline=True),
                tile(e("number", "setpoint_comfort"), "Setpoint Comfort", "mdi:thermometer", full=True,
                     features=NUMBER_BUTTONS, inline=True),
                tile(e("number", "setpoint_economy"), "Setpoint Economy", "mdi:thermometer-low", full=True,
                     features=NUMBER_BUTTONS, inline=True),
                tile(e("sensor", "temperatura_mandata_richiesta"), "Mandata richiesta", "mdi:thermometer-check",
                     full=True),
            ],
        },
        {
            "type": "grid",
            "column_span": 2,
            "cards": [
                {"type": "heading", "heading": "Flussi d'aria", "icon": "mdi:swap-horizontal-bold"},
                schema,
                grafico,
            ],
        },
        {
            "type": "grid",
            "cards": [
                {"type": "heading", "heading": "Filtri e allarmi", "icon": "mdi:air-filter"},
                tile(e("sensor", "filtri_giorni_residui"), "Filtri", "mdi:air-filter"),
                press(e("button", "reset_timer_filtri"), "Reset filtri", "mdi:restore",
                      "Azzerare il timer dei filtri? Fallo solo dopo averli cambiati."),
                tile(e("sensor", "allarmi_attivi"), "Allarmi attivi", "mdi:alert-outline", full=True),
                tile(e("binary_sensor", "allarme_critico"), "Allarme critico", "mdi:alert-octagon"),
                tile(e("binary_sensor", "avviso_attivo"), "Avviso", "mdi:alert"),
                press(e("button", "reset_allarmi"), "Reset allarmi", "mdi:alarm-light-off-outline",
                      "Resettare gli allarmi dell'unità?"),
            ],
        },
        {
            "type": "grid",
            "cards": [
                {"type": "heading", "heading": "Impostazioni", "icon": "mdi:cog"},
                tile(e("select", "inverno_estate"), "Stagione", "mdi:sun-snowflake-variant", full=True,
                     features=SELECT),
                tile(e("binary_sensor", "stagione_riscaldamento"), "Riscaldamento", "mdi:snowflake-thermometer"),
                tile(e("number", "durata_boost"), "Durata boost", "mdi:timer-cog-outline"),
                tile(e("number", "setpoint_protezione_edificio"), "Setpoint protezione edificio",
                     "mdi:home-thermometer-outline", full=True, features=NUMBER_BUTTONS, inline=True),
                tile(e("switch", "raffrescamento_notturno"), "Raffrescamento notturno", "mdi:weather-night",
                     full=True, features=TOGGLE, inline=True),
                tile(e("switch", "protezione_secchezza"), "Protezione secchezza", "mdi:water-off", full=True,
                     features=TOGGLE, inline=True),
                tile(e("number", "soglia_protezione_secchezza"), "Soglia secchezza", "mdi:water-percent",
                     full=True, features=NUMBER_BUTTONS, inline=True),
                tile(e("switch", "portata_in_base_a_ur_ripresa"), "Portata in base all'umidità", "mdi:water-percent",
                     full=True, features=TOGGLE, inline=True),
                tile(e("number", "setpoint_ur_inverno"), "UR inverno", "mdi:water-percent"),
                tile(e("number", "setpoint_ur_estate"), "UR estate", "mdi:water-percent"),
            ],
        },
        {
            "type": "grid",
            "cards": [
                {"type": "heading", "heading": "Dispositivo", "icon": "mdi:chip"},
                tile(e("binary_sensor", "connessione_esp"), "ESP", "mdi:lan-connect"),
                tile(e("binary_sensor", "pannello_s_touch_attivo"), "Pannello S-Touch", "mdi:tablet-dashboard"),
                tile(e("sensor", "segnale_wifi"), "Wi-Fi", "mdi:wifi"),
                tile(e("sensor", "ritrasmissioni_esp"), "Ritrasmissioni", "mdi:repeat"),
                tile(e("sensor", "richieste_esp_fallite"), "Richieste fallite", "mdi:close-circle-outline"),
                tile(e("sensor", "versione_software"), "Firmware unità", "mdi:information-outline"),
                press(e("button", "sincronizza_orologio"), "Sincronizza orologio", "mdi:clock-check-outline"),
                press(e("button", "riavvia_esp"), "Riavvia ESP", "mdi:restart", "Riavviare l'ESP della VMC?"),
            ],
        },
    ],
}

(HERE / "vmc-view.json").write_text(json.dumps(view, ensure_ascii=False, indent=1), encoding="utf-8")
(HERE / "vmc-view.yaml").write_text(
    "# Vista \"VMC\" per una dashboard a sezioni. Generata da genera_vista_vmc.py: non modificare a mano.\n"
    + yaml.dump(view, allow_unicode=True, sort_keys=False, width=100000),
    encoding="utf-8",
)
print("scritti vmc-view.yaml e vmc-view.json")
