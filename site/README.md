# site/

Die Seite unter <https://esp32can.thomas-peterson.de>, über die sich die
Firmware direkt aus dem Browser aufspielen lässt (Web Serial API).

Nicht unter `web/` ablegen: `tools/build_fs.py` packt `web/` rekursiv in die
4-MB-Datenpartition des Geräts. Diese Seite gehört auf einen Webserver, nicht
in den Flash der Heizungssteuerung.

## Aufbau nach dem Ausrollen

    index.html
    manifest-release.json     aus manifest.json.in, @VERSION@/@CHANNEL@ ersetzt
    manifest-dev.json
    bin/release/*.bin         die fünf Abschnitte des jeweiligen Standes
    bin/dev/*.bin

## Offsets

Aus `build/flasher_args.json`; in JSON zwingend dezimal, weil JSON keine
Hexadezimalzahlen kennt.

| Datei | Offset | dezimal |
|---|---|---|
| `bootloader.bin` | `0x0` | 0 |
| `partition-table.bin` | `0x8000` | 32768 |
| `ota_data_initial.bin` | `0xF000` | 61440 |
| `open3e-gateway.bin` | `0x20000` | 131072 |
| `storage.bin` | `0x820000` | 8519680 |

Ein falscher Offset erzeugt kein Fehlerbild, sondern ein Gerät, das nicht
startet. Nach jeder Änderung an `partitions.csv` gehören diese Zahlen geprüft.

## Bauen und ausrollen

    make site      # Firmware bauen und site/ nach build/site/ zusammenstellen
    make deploy    # das Ergebnis auf den Webserver schieben

`make site` erzeugt beide Kanäle aus demselben Build; `CHANNEL=dev make site`
legt ihn unter `bin/dev/` ab.

## Voraussetzungen auf dem Server

Web Serial verlangt **HTTPS** — ohne gültiges Zertifikat bleibt der Knopf
wirkungslos. Einzige Ausnahme ist `http://localhost`, weshalb sich die Seite
lokal ohne Zertifikat testen lässt:

    make site && (cd build/site && python3 -m http.server 8000)

Die `.bin`-Dateien müssen unverändert ausgeliefert werden.
