/* Die Topic-Expansion der Weboberfläche gegen mqtt_pub_topic().
 *
 * web/app.js baut die Topics nach, die die Firmware veröffentlicht, weil die
 * erzeugte evcc-Konfiguration sie beim Namen nennen muss. Zwei
 * Implementierungen derselben Regel laufen auseinander -- und zwar lautlos:
 * ein Topic, das es nicht gibt, liefert in evcc einfach nie einen Wert, ohne
 * Fehlermeldung auf irgendeiner Seite.
 *
 * Maßgeblich ist mqtt_pub_topic() in main/mqtt_pub.c. Die Fälle unten sind von
 * dort abgelesen; die letzten fünf sind zusätzlich an einer laufenden Anlage
 * gegen die tatsächlich am Broker anliegenden Topics geprüft worden.
 */
const fs = require("fs");
const path = require("path");

const src = fs.readFileSync(path.join(__dirname, "..", "web", "app.js"), "utf8");
const m = src.match(
  /\/\* @topic-expansion-start[\s\S]*?\*\/([\s\S]*?)\/\* @topic-expansion-end \*\//);
if (!m) {
  console.error("FAILED: die Marker @topic-expansion-start/-end fehlen in web/app.js");
  process.exit(1);
}
eval(m[1]);

let fail = 0;
function check(what, got, want) {
  if (got !== want) {
    fail++;
    console.log(`  FAIL ${what}\n       ist  ${got}\n       soll ${want}`);
  }
}

/* base, format, ecu, did, didName, device, override */
const T = (f, o) => expandTopic("open3e", f, 0x6a1, 1836, "StoragePower", "VX3", o);

/* Eine Überschreibung ersetzt den formatierten Teil, nicht die Basis. */
check("Überschreibung", T("{didName}", "eigenes/Topic"), "open3e/eigenes/Topic");
check("Überschreibung schlägt Format", T("{ecuAddr}", "x"), "open3e/x");

/* Die Platzhalter, die open3es README dokumentiert. */
check("didName", T("{didName}"), "open3e/StoragePower");
check("didNumber", T("{didNumber}"), "open3e/1836");
check("didNumber:04d", T("{didNumber:04d}"), "open3e/1836");
check("didNumber:06d", T("{didNumber:06d}"), "open3e/001836");
check("ecuAddr (dezimal)", T("{ecuAddr}"), "open3e/1697");
check("ecuAddr:03X", T("{ecuAddr:03X}"), "open3e/6A1");
check("ecuAddr:03x", T("{ecuAddr:03x}"), "open3e/6a1");
check("device", T("{device}"), "open3e/VX3");
check("kombiniert", T("{ecuAddr:03X}/{didName}"), "open3e/6A1/StoragePower");
check("Text dazwischen", T("a-{didNumber}-b"), "open3e/a-1836-b");

/* Ein unbekannter Platzhalter wird durchkopiert, damit ein Tippfehler im
 * Topic sichtbar wird statt zu verschwinden -- so macht es die Firmware. */
check("unbekannter Platzhalter", T("{didname}"), "open3e/{didname}");
check("nicht geschlossen", T("{didName"), "open3e/{didName");
/* Ein leeres Format kann das Gerät nicht liefern: sys_cfg_set() speichert
 * einen leeren Formatstring als "{didName}". Die Ausweichregel im Browser
 * spiegelt genau diese Normalisierung -- und dieser Fall hielt beim ersten
 * Lauf die falsche Erwartung fest, nämlich meine Vermutung über C-Verhalten,
 * das es so nie gibt. */
check("leeres Format weicht auf didName aus", T(""), "open3e/StoragePower");

/* Schmale Führungsnull: nur eine 0-präfixierte Angabe ist eine Breite. */
check("didNumber:d ohne Null", T("{didNumber:d}"), "open3e/1836");

/* ---- an einer laufenden Anlage gegen den Broker geprüft ---- */
const REAL = [
  { f: "{ecuAddr:03X}/{didName}", ecu: 1697, did: 2188,
    name: "PointOfCommonCouplingSetActivePowerTotal", ov: "",
    want: "open3e/6A1/PointOfCommonCouplingSetActivePowerTotal" },
  { f: "{ecuAddr:03X}/{didName}", ecu: 1697, did: 1836,
    name: "ElectricalEnergyStorageCurrentPower",
    ov: "VitochargeVX3_6A1_1836_ElectricalEnergyStorageCurrentPower",
    want: "open3e/VitochargeVX3_6A1_1836_ElectricalEnergyStorageCurrentPower" },
  { f: "{ecuAddr:03X}/{didName}", ecu: 1697, did: 1664,
    name: "ElectricalEnergyStorageStateOfCharge",
    ov: "VitochargeVX3_6A1_1664_ElectricalEnergyStorageStateOfCharge",
    want: "open3e/VitochargeVX3_6A1_1664_ElectricalEnergyStorageStateOfCharge" },
];
for (const r of REAL) {
  check(`echte Anlage, DID ${r.did}`,
        expandTopic("open3e", r.f, r.ecu, r.did, r.name, "", r.ov), r.want);
}

console.log(fail ? `FAILED: ${fail} Abweichung(en)` : "topic: Topic-Expansion der Weboberfläche");
process.exit(fail ? 1 : 0);
