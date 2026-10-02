# open3e auf dem ESP32 (Waveshare ESP32-S3-RS485-CAN)

Eine Firmware, die E3-Geräte direkt am CAN-Bus ausliest und nach MQTT bringt —
auf einem Board für rund 20 €, das an 7–36 V direkt im Heizungsraum läuft.
Kein Raspberry Pi, kein Python, kein SocketCAN.

Zur Masse: das Board sieht eine galvanische Trennung vor, überbrückt sie aber
im Auslieferungszustand mit **R31, einem 0-Ω-Widerstand**. Die Board-Masse ist
damit der CAN-Bezugspunkt und muss an die Busmasse — ohne sie steigt der
TX-Fehlerzähler, während RX bei null bleibt. Hängt das Board am USB eines
geerdeten Rechners, verbindet man zwei Erdungspunkte; dann vorher auf ein
eigenes Netzteil wechseln.

Projektseite: <https://github.com/boonkerz/open3e-esp32>

## Verhältnis zu open3e

Das ist kein eigenes Protokoll und kein Konkurrenzprodukt, sondern open3e auf
anderer Hardware:

- Die **Codecs sind ein Port** von Open3Ecodecs.py nach C.
- Die **Datenpunktdatenbank kommt aus open3e** und wird beim Bauen aus dem
  Repository erzeugt (derzeit 1564 Datenpunkte).
- **Topics, Payloads, LWT und Kommando-Topic folgen Open3Eclient.py**,
  einschließlich der Formatstring-Platzhalter `{didName}`, `{didNumber}`,
  `{ecuAddr}` und `{device}`.

Ein bestehendes Broker-Setup, ein Home-Assistant-Template oder eine
Node-RED-Strecke läuft also unverändert weiter. Wer vom Pi umzieht, tauscht die
Hardware, nicht die Integration.

## Warum man den Werten trauen kann

Das ist die eigentliche Frage bei einer Neuimplementierung: eine Abweichung im
Codec merkt man nicht am Fehler, sondern erst daran, dass ein Wert still falsch
ist.

Deshalb werden die Testvektoren nicht von Hand geschrieben, sondern erzeugt,
indem **open3es eigenes Python läuft**. Verglichen wird byteweise:

| Prüfung | Ergebnis |
|---|---|
| Dekodieren == open3e | 4692 / 4692 Vektoren über alle 1564 DIDs |
| Kodieren == open3e | 3990 / 3990 Vektoren |
| … und dieselben Ablehnungen | 332 / 332 |
| geflachtes MQTT == `mqttdump()` | 4692 Datenpunkte, 23 395 Topic/Wert-Paare, 0 Abweichungen |

Dazu kommen eigene Suiten für ISO-TP (Segmentierung, Flow Control,
SN-Überlauf, Frame-Verlust bis 4095 Byte) und für UDS — letztere sichert
ausdrücklich die Eigenschaft, dass eine Anfrage, die **nicht** oder negativ
beantwortet wird, keinen Erfolg meldet.

Die E380-Dekodierung ist aus [E3onCAN](https://github.com/MyHomeMyData/E3onCAN)
portiert und wird gegen dessen Ausgabe geprüft (560 / 560 Vektoren über 14
CAN-IDs).

## Ausprobieren

Zum Ausprobieren braucht es **keine Toolchain**. Die Firmware lässt sich aus
dem Browser aufspielen — über Web Serial, also mit Chrome, Edge, Opera,
Firefox ab 151 oder Chrome für Android. Mit Safari und auf iOS geht es nicht,
weil WebKit die Schnittstelle nicht umsetzt:

<https://esp32can.thomas-peterson.de>

Danach:

1. Das Gerät spannt einen WLAN-Hotspot auf, das Captive Portal öffnet die
   Einrichtungsseite von selbst.
2. WLAN eintragen → Neustart → erreichbar unter `http://open3e.local`.
3. Im Web-UI einen Bus-Scan starten; gefundene Geräte und Datenpunkte
   erscheinen mit Namen aus der open3e-Datenbank.
4. Pro Datenpunkt festlegen, ob und auf welches Topic er geht, mit welchem
   Intervall, als JSON oder flach.

## Was darüber hinaus geht

MQTT-Auto-Discovery für Home Assistant (mit Einheiten und Geräteklassen aus
dem Codec), passiver Empfang des E380-Energiezählers, Dekodierung des
Broadcast-Kanals (Service 0x77, auf dem bei einem Vitocharge der Verkehr
zwischen Backend-Gateway und Speicher läuft), ein CAN-Mitschnitt im Browser
mit Auslöser auf Schreibzugriffe, zwei Kontakteingänge auf den freien GPIO des
SH1.0-Steckers (Klingel, Türkontakt), KNXnet/IP, ein Erzeuger für
evcc-Konfiguration — und das netzdienstliche Laden einer Vitocharge VX3 über
DID 2188.

Einzelheiten stehen in der README des Projekts; hier würden sie den Rahmen
sprengen.

## Rohdaten für eigene Decoder

`GET /api/rawread` und `POST /api/rawwrite` liefern und schreiben
UDS-Nutzdaten **ohne** den open3e-Codec, und ausgewählte CAN-IDs lassen sich
roh nach `<Basis>/raw/<id>` weiterreichen. Gedacht für Software mit eigener
Datenpunktdatenbank — namentlich ioBroker.e3oncan im Gateway-Betrieb.

Diese Schnittstelle stammt aus einem
[Pull Request von MyHomeMyData](https://github.com/boonkerz/open3e-esp32/pull/1)
und ist seine Arbeit, nicht meine.

## Was es nicht kann

- **RS485** — die Hardware ist da, die Firmware nutzt sie nicht.
- **DoIP** als Alternative zu CAN.
- **Schreiben über Service 0x77.** Gelesen und dekodiert wird er vollständig;
  selbst schreiben ginge nur gegen ein Gateway, das dieselben Datenpunkte alle
  zehn Sekunden neu setzt.
- **KNX Secure** — eine gesicherte Installation ignoriert, was gesendet wird.
- **Ein ESP32 ist kein Linux.** Wer neben open3e eigene Skripte, eine
  Datenbank oder weitere Adapter auf derselben Kiste laufen lässt, ist mit dem
  Raspberry Pi besser bedient. Dieses Gerät macht eine Sache.

## Lizenz und Herkunft

Der Code in `main/` ist ein eigenständiger Port. Die Datenpunktdefinitionen,
Enumerationen und die Codec-Semantik stammen aus open3e und stehen unter der
**Apache License 2.0** (Copyright 2023 abnoname, philippoo66 und Mitwirkende);
Einzelheiten in der `NOTICE` des Projekts. Die open3e-Quellen liegen nicht im
Repository — `tools/fetch_open3e.py` holt sie beim Bauen auf einen festen
Commit und `tools/gen_dpdb.py` wandelt sie in die Datenbank auf dem Gerät.
