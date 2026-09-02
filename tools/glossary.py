#!/usr/bin/env python3
"""German reading aids for the open3e datapoint names.

open3e itself describes only about one datapoint in ten, so most names have to
speak for themselves -- and they are English CamelCase built from a small
vocabulary: 200 words cover 87% of all occurrences. Translating that vocabulary
and reassembling gives a usable German label for nearly every datapoint.

This is a reading aid, not a translation: German compounds do not always follow
the English word order, so a few labels read oddly. The original name is always
shown alongside, and it stays the identifier everything else uses.

Multi-word phrases are matched first and longest-first, because that is where
the good renderings are: "DomesticHotWater" is "Trinkwarmwasser", not
"Häuslich Heiß Wasser".
"""
import re

# Longest-first phrase matches. Order within the dict does not matter; the
# matcher sorts by length.
PHRASES = {
    "DomesticHotWater": "Trinkwarmwasser",
    "CentralHeating": "Zentralheizung",
    "StateOfCharge": "Ladezustand",
    "StateOfEnergy": "Energiezustand",
    "StateOfHealth": "Alterungszustand",
    "HeatPump": "Wärmepumpe",
    "HeatExchanger": "Wärmetauscher",
    "HeatingCurve": "Heizkurve",
    "FlowTemperature": "Vorlauftemperatur",
    "ReturnTemperature": "Rücklauftemperatur",
    "OutsideTemperature": "Außentemperatur",
    "RoomTemperature": "Raumtemperatur",
    "ExhaustTemperature": "Abgastemperatur",
    "WaterPressure": "Wasserdruck",
    "ElectricalEnergyStorage": "Batteriespeicher",
    "ElectricalEnergySystem": "Energiesystem",
    "ElectricEnergyStorage": "Batteriespeicher",
    "ThreePhaseInverter": "Dreiphasen-Wechselrichter",
    "EnergyProduction": "Energieerzeugung",
    "EnergyConsumption": "Energieverbrauch",
    "GasConsumption": "Gasverbrauch",
    "PowerConsumption": "Leistungsaufnahme",
    "OperatingData": "Betriebsdaten",
    "OperationMode": "Betriebsart",
    "OperatingMode": "Betriebsart",
    "QuickMode": "Schnellfunktion",
    "HolidayProgram": "Urlaubsprogramm",
    "HolidayAtHome": "Urlaub zu Hause",
    "FrostProtection": "Frostschutz",
    "LegionellaProtection": "Legionellenschutz",
    "SmartGridReady": "SG-Ready",
    "SolarThermal": "Solarthermie",
    "Photovoltaic": "Photovoltaik",
    "BusIdentification": "Bus-Kennung",
    "SoftwareVersion": "Softwarestand",
    "HardwareVersion": "Hardwarestand",
    "SerialNumber": "Seriennummer",
    "ErrorHistory": "Fehlerhistorie",
    "StatusMessage": "Statusmeldung",
    "TimeSchedule": "Zeitprogramm",
    "SetpointMetaData": "Sollwert-Grenzen",
    "MinMaxAverage": "Min/Max/Mittel",
    "CrankCase": "Kurbelgehäuse",
    "RefrigerantCycle": "Kältekreis",
    "CircuitOne": "Heizkreis 1",
    "CircuitTwo": "Heizkreis 2",
    "CircuitThree": "Heizkreis 3",
    "CircuitFour": "Heizkreis 4",
    "MixerOne": "Mischer 1",
    "MixerTwo": "Mischer 2",
    "MixerThree": "Mischer 3",
    "MixerFour": "Mischer 4",
    "ZigBee": "ZigBee",
    "EEBus": "EEBus",
    # "Current" is ambiguous: electrical current, or "the current one". The
    # phrases below are the second sense; a bare "Current" stays "Strom".
    "CurrentPower": "Aktuelle Leistung",
    "CurrentValue": "Aktueller Wert",
    "CurrentState": "Aktueller Zustand",
    "CurrentMode": "Aktuelle Betriebsart",
    "CurrentQuickMode": "Aktuelle Schnellfunktion",
    "CurrentTemperature": "Aktuelle Temperatur",
    "CurrentSetpoint": "Aktueller Sollwert",
    # German writes these as one word; joining with spaces reads badly.
    "TemperatureSensor": "Temperaturfühler",
    "TemperatureSetpoint": "Temperatursollwert",
    "PressureSensor": "Drucksensor",
    "HumiditySensor": "Feuchtefühler",
    "OperatingHours": "Betriebsstunden",
    "OperatingState": "Betriebszustand",
    "StatusMessages": "Statusmeldungen",
    "ErrorMessages": "Fehlermeldungen",
    "PowerConsumptionStatistical": "Stromverbrauch-Statistik",
    "EnergyTransferStatistic": "Energiebilanz",
    "ControlMode": "Regelungsart",
    "TargetQuickMode": "Schnellfunktion anfordern",
    "InverterCurrentPower": "Wechselrichter-Leistung",
}

WORDS = {
    # structure
    "Circuit": "Heizkreis", "Mixer": "Mischer", "Room": "Raum",
    "Apartment": "Wohnung", "Zone": "Zone", "Module": "Modul",
    "Modul": "Modul", "Device": "Gerät", "System": "System",
    "Cascade": "Kaskade", "Group": "Gruppe", "Board": "Platine",
    "Terminal": "Klemme", "Channel": "Kanal", "Bus": "Bus",
    "Gateway": "Gateway", "Network": "Netzwerk", "Ethernet": "Ethernet",
    "Internet": "Internet", "Radio": "Funk", "Accessory": "Zubehör",
    "Object": "Objekt", "Unit": "Einheit", "Engine": "Maschine",
    "Home": "Haus", "Base": "Basis", "Common": "Allgemein",
    "Generic": "Allgemein", "Universal": "Universal", "Series": "Serie",
    # heating parts
    "Heater": "Heizgerät", "Burner": "Brenner", "Flame": "Flamme",
    "Compressor": "Verdichter", "Pump": "Pumpe", "Valve": "Ventil",
    "Fan": "Lüfter", "Ventilation": "Lüftung", "Filter": "Filter",
    "Buffer": "Puffer", "Storage": "Speicher", "Battery": "Batterie",
    "Cell": "Zelle", "Inverter": "Wechselrichter", "Meter": "Zähler",
    "Sensor": "Fühler", "Thermostat": "Thermostat", "Actuator": "Stellantrieb",
    "Drive": "Antrieb", "Diverter": "Umschaltventil", "Bypass": "Bypass",
    "Exchanger": "Tauscher", "Hydraulic": "Hydraulisch",
    "Circulation": "Zirkulation", "Coupling": "Kopplung",
    "Expansion": "Erweiterung", "Switch": "Schalter", "Relay": "Relais",
    "Solar": "Solar", "Grid": "Netz", "Fuel": "Brennstoff", "Gas": "Gas",
    "Air": "Luft", "Water": "Wasser", "Liquid": "Flüssigkeit",
    "Refrigerant": "Kältemittel", "Flue": "Abgas", "Exhaust": "Abgas",
    "Outlet": "Austritt", "Inlet": "Eintritt", "Supply": "Zufuhr",
    "Return": "Rücklauf", "Flow": "Vorlauf", "Source": "Quelle",
    # measurements
    "Temperature": "Temperatur", "Pressure": "Druck", "Humidity": "Feuchte",
    "Power": "Leistung", "Energy": "Energie", "Electricity": "Strom",
    "Current": "Strom", "Voltage": "Spannung", "Volume": "Volumen",
    "Speed": "Drehzahl", "Level": "Stand", "Percent": "Prozent",
    "Rate": "Rate", "Capacity": "Kapazität", "Load": "Last",
    "Consumption": "Verbrauch", "Production": "Erzeugung",
    "Performance": "Leistungszahl", "Modulation": "Modulation",
    "Reactive": "Blind", "Nominal": "Nenn", "Relative": "Relativ",
    "Thermal": "Thermisch", "Electrical": "Elektrisch", "Electric": "Elektrisch",
    "Electronic": "Elektronisch", "Analog": "Analog", "Digital": "Digital",
    "Condensing": "Kondensations", "Evaporating": "Verdampfungs",
    "Noise": "Geräusch", "Price": "Preis", "Coverage": "Deckung",
    # control
    "Setpoint": "Sollwert", "Setpoints": "Sollwerte", "Target": "Soll",
    "Value": "Wert", "Values": "Werte", "Actual": "Ist",
    "Mode": "Betriebsart", "State": "Zustand", "Status": "Status",
    "Control": "Regelung", "Configuration": "Konfiguration",
    "Settings": "Einstellungen", "Setting": "Einstellung",
    "Set": "Setzen", "Limit": "Grenze", "Limitation": "Begrenzung",
    "Threshold": "Schwelle", "Hysteresis": "Hysterese", "Offset": "Versatz",
    "Curve": "Kurve", "Position": "Stellung", "Enable": "Freigabe",
    "Activation": "Aktivierung", "Active": "Aktiv", "Demand": "Anforderung",
    "Request": "Anforderung", "Function": "Funktion", "Features": "Funktionen",
    "Protection": "Schutz", "Reduction": "Absenkung", "Saving": "Sparen",
    "Reset": "Zurücksetzen", "Start": "Start", "Stop": "Stopp",
    "Quick": "Schnell", "Fixed": "Fest", "Maximum": "Maximum",
    "Minimum": "Minimum", "Max": "Max", "Min": "Min", "High": "Hoch",
    "Low": "Niedrig", "Mid": "Mittel", "Bottom": "Unten", "Top": "Oben",
    "Primary": "Primär", "Secondary": "Sekundär", "External": "Extern",
    "Internal": "Intern", "Remote": "Fern", "Smart": "Smart",
    "Alternating": "Wechselnd", "Available": "Verfügbar",
    "Supported": "Unterstützt", "Visible": "Sichtbar",
    # time
    "Time": "Zeit", "Date": "Datum", "Schedule": "Zeitprogramm",
    "Timer": "Timer", "Duration": "Dauer", "Interval": "Intervall",
    "Runtime": "Laufzeit", "Run": "Lauf", "Hours": "Stunden",
    "Month": "Monat", "Year": "Jahr", "Last": "Letzte", "Cycle": "Zyklus",
    "Steps": "Stufen", "Phase": "Phase", "Lag": "Verzögerung",
    "History": "Historie", "Statistics": "Statistik",
    "Statistical": "Statistisch", "Recorded": "Aufgezeichnet",
    "Monitoring": "Überwachung", "Monday": "Montag", "Tuesday": "Dienstag",
    "Wednesday": "Mittwoch", "Thursday": "Donnerstag", "Friday": "Freitag",
    "Saturday": "Samstag", "Sunday": "Sonntag", "Holiday": "Urlaub",
    # diagnostics
    "Error": "Fehler", "Alarm": "Alarm", "Warning": "Warnung",
    "Service": "Wartung", "Diagnostic": "Diagnose", "Test": "Test",
    "Message": "Meldung", "Code": "Code", "Result": "Ergebnis",
    "Information": "Information", "Info": "Info", "Data": "Daten",
    "Identification": "Kennung", "Name": "Name", "Number": "Nummer",
    "Version": "Version", "Type": "Typ", "Property": "Eigenschaft",
    "Meta": "Meta", "List": "Liste", "Matrix": "Matrix", "Point": "Punkt",
    "Topology": "Topologie", "Process": "Prozess", "Signal": "Signal",
    "Input": "Eingang", "Output": "Ausgang", "Access": "Zugriff",
    "Operation": "Betrieb", "Operating": "Betriebs", "Dtc": "Fehlercode",
    "Defrost": "Abtauung", "Frost": "Frost", "Legionella": "Legionellen",
    "Heating": "Heizen", "Cooling": "Kühlen", "Heat": "Wärme",
    "Hot": "Warm", "Domestic": "Trinkwasser", "Central": "Zentral",
    "Charge": "Ladung", "Generated": "Erzeugt", "Case": "Gehäuse",
    "Crank": "Kurbel", "Pre": "Vor", "Of": "", "And": "und", "In": "in",
    "At": "bei", "On": "ein", "Way": "Wege",
    # long tail: each of these occurs only a handful of times, but leaving
    # them in English mid-sentence is what makes a label read badly
    "Default": "Standard", "Details": "Details", "Support": "Unterstützung",
    "Acknowledge": "Quittieren", "Blocked": "Gesperrt", "Summer": "Sommer",
    "Winter": "Winter", "Application": "Anwendung", "Safety": "Sicherheit",
    "Product": "Produkt", "Controller": "Regler", "Oscillation": "Schwingung",
    "Lock": "Sperre", "Own": "Eigen", "Selection": "Auswahl",
    "Norm": "Norm", "Refrigeration": "Kälte", "Coefficient": "Koeffizient",
    "Constant": "Konstant", "Eco": "Eco", "Seasonal": "Saisonal",
    "Self": "Eigen", "Delay": "Verzögerung", "Optimum": "Optimum",
    "Duct": "Kanal", "Separator": "Weiche", "Outdoor": "Außen",
    "Next": "Nächste", "Post": "Nach", "Savings": "Einsparung",
    "Delivery": "Lieferung", "Part": "Teil", "Proxy": "Proxy",
    "Up": "Auf", "Down": "Ab", "Band": "Band", "Narrow": "Schmal",
    "Weather": "Wetter", "Resulting": "Resultierend", "Increase": "Erhöhung",
    "Decrease": "Verringerung", "Managment": "Management",
    "Management": "Management", "Comfort": "Komfort", "Standard": "Standard",
    "Reduced": "Reduziert", "Normal": "Normal", "Party": "Party",
    "Economy": "Sparbetrieb", "Manual": "Manuell", "Automatic": "Automatik",
    "Emergency": "Notbetrieb", "Standby": "Bereitschaft",
    # ordinals
    "One": "1", "Two": "2", "Three": "3", "Four": "4", "Five": "5",
    "Six": "6", "Seven": "7", "Eight": "8", "Nine": "9", "Ten": "10",
    "Eleven": "11", "Twelve": "12", "Thirteen": "13", "Fourteen": "14",
    "Fifteen": "15", "Sixteen": "16", "Seventeen": "17", "Eighteen": "18",
    "Nineteen": "19", "Twenty": "20", "Thirty": "30",
}

_SPLIT = re.compile(r"[A-Z][a-z]+|[A-Z]{2,}(?![a-z])|\d+")


def translate(name: str) -> str:
    """Render one datapoint name in German. Returns "" if nothing is known."""
    rest = name
    parts = []
    hit = False

    while rest:
        # Longest phrase that starts here wins.
        for phrase in sorted(PHRASES, key=len, reverse=True):
            if rest.startswith(phrase):
                parts.append(PHRASES[phrase])
                rest = rest[len(phrase):]
                hit = True
                break
        else:
            m = _SPLIT.match(rest)
            if not m:
                rest = rest[1:]
                continue
            word = m.group(0)
            rest = rest[len(word):]
            if word in WORDS:
                if WORDS[word]:
                    parts.append(WORDS[word])
                hit = True
            else:
                parts.append(word)   # untranslated, kept so the label still reads

    if not hit:
        return ""
    return " ".join(p for p in parts if p)


if __name__ == "__main__":
    import json
    import sys
    from pathlib import Path

    src = Path(__file__).resolve().parent.parent / ".cache/open3e/src/open3e/Open3Edatapoints.json"
    d = json.loads(src.read_text())
    dps = {int(k): v for k, v in d.items() if isinstance(v, dict)}

    translated = sum(1 for v in dps.values() if translate(v["id"]))
    print(f"{translated}/{len(dps)} datapoints get a German label "
          f"({100 * translated / len(dps):.0f}%)")
    for did in sorted(dps)[:0] or [268, 274, 1836, 1834, 1577, 396, 2225, 2239]:
        if did in dps:
            print(f"  {did:5d}  {dps[did]['id']:<46} {translate(dps[did]['id'])}")
