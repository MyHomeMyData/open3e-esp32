Gerne — und danke für das Angebot.

**Stand:** Die Firmware läuft hier seit Monaten an zwei Anlagen im Dauerbetrieb
(Vitocal 250 mit Vitocharge VX3 und E380 am einen Strang, Vitovent am anderen).
Dekodieren und Kodieren werden gegen open3es eigenes Python geprüft — die
Vektoren entstehen, indem Open3Ecodecs.py läuft: 4692/4692 Vektoren über alle
1564 DIDs, 3990/3990 beim Kodieren samt derselben 332 Ablehnungen, und 23 395
Topic/Wert-Paare gegen `mqttdump()` ohne Abweichung. Dazu eigene Suiten für
ISO-TP, UDS, den Broadcast-Kanal und den E380 (gegen E3onCAN).

Aufspielen geht ohne Toolchain aus dem Browser, danach Hotspot → WLAN →
`open3e.local` → Bus-Scan im Web-UI.

Den Wiki-Artikel schreibe ich. Eine Frage zur Einordnung: das Wiki nummeriert
die Seiten, und wo die neue hingehört, ist Deine Entscheidung. Naheliegend
fände ich einen Platz direkt neben *020 Inbetriebnahme CAN Adapter am
Raspberry*, weil der Artikel genau deren Alternative beschreibt — etwa als
*021*. Sag mir die gewünschte Nummer und den Titel, dann lege ich die Seite
entsprechend an.

Noch ein Hinweis in eigener Sache: die Rohdaten-Schnittstelle
(`/api/rawread`, `/api/rawwrite`, das MQTT-Roh-Relay) stammt aus Deinem
eigenen Pull Request. Das steht im Artikel auch so drin.

Projektseite: https://github.com/boonkerz/open3e-esp32
Flashen im Browser: https://esp32can.thomas-peterson.de
