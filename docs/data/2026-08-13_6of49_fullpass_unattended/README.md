# 2026-08-13 — 6-of-49, full pass, UNATTENDED (5005/5005)

Unbeaufsichtigt (`focus=off`), `score=high`, `run=5 s` / `gap=2 s`, 49 Blöcke,
**37,90 h**, keine Pause, `void=0`. Firmware `68d4ffa-dirty`, ELF SHA
`975bc993eefaec17` — der Zwischenstand vom 11.08., also **vor** der blockweisen
Zentrierung. Alle Zahlen hier sind damit uncentriert.

⚠ Diese Daten lagen bis zum 17.08. nur in einem Temp-Verzeichnis. Der Master
wurde seither mehrfach neu geflasht; im RAM ist der Lauf längst weg.

⚠ **Die Kommentar-Kopfzeile von `results.csv` ist abgeschnitten** und trägt
dahinter ein paar Fremdbytes: der Header lief über seinen 224-Byte-Puffer, und
die Länge ging ungeprüft an `httpd_resp_send_chunk()` (behoben 2026-08-17).
Verloren ist dadurch `drift_t` und das `# nodes=`-Präfix; die IP-Liste in
Spaltenreihenfolge steht noch da und ist gültig. **Die Datenzeilen sind nicht
betroffen** — sie blieben immer deutlich unter der Puffergröße. Dasselbe gilt
für `_live_*` und `_short_*`.

| Datei | Inhalt |
|---|---|
| `results.csv` | der Datensatz — 5005 Items, roh, in Messreihenfolge, mit `z0..z3` pro Node |
| `summary.csv` | die 15 veröffentlichten Zeilen |
| `status.json` | `/status` am Ende |
| `loops.json` | 49 Blöcke, Gesundheit je Block |

## Ergebnis: null

Nach blockweiser Korrektur max |z| = **3,92** bei **4,13** erwartet als Maximum
von 5005 Standardnormalwerten. Nichts vorhanden.

## Was der Lauf über das Instrument sagt

**Innerhalb der Blöcke ist alles sauber, zwischen den Blöcken springt der
Nullpunkt.** Per-Node-σ liegt blockintern fast überall bei ~1,0; über den ganzen
Lauf aber:

| Node | mean | σ (gesamt) | Blöcke mit \|Blockmittel\| > 1,5 | schlimmster Block |
|---|---|---|---|---|
| master | −0,846 | 1,753 | **9 von 49** | −6,33 (Blk 4) |
| slave1 (.145) | +0,968 | **3,678** | **7 von 49** | **+24,13 (Blk 11)** |
| slave2 (.155) | −0,104 | 1,062 | 0 von 49 | −0,86 |
| slave0 (.103) | −0,010 | 1,083 | 0 von 49 | +1,10 |

Pass-σ 1,378 bei blockintern gepooltem σ ~1,0 — der gesamte Überschuss ist der
Blockversatz, nicht Korrelation (schlechtestes paarweises |r| 0,024).
`null_flags=5` (σ- und χ²-Gate) hat korrekt angeschlagen.

**Top-5 und Bottom-5 sind Artefakte:** alle fünf Top-Werte aus Block 42 (slave1
mit σ 4,68 dort), vier von fünf Bottom-Werten aus Block 4 (master bei −6,33).

## Die zwei Konstruktionsfehler, die dieser Lauf freigelegt hat

1. **Die Untergrenze von 3 Nodes ließ nur EINEN Ausschluss zu.** slave1 flog
   raus, der in denselben Blöcken schlechtere master blieb drin. Block 4 rechnet
   sich exakt nach: (−6,33 + 0,27 + 0,22)/√3 = −3,38, genau der publizierte Wert.
2. **Quarantäne feuerte nur beim ERSTEN Auslösen** eines Nodes, deshalb wurden
   nur die Blöcke 1, 24 und 33 gesperrt (309 Items), während 4, 14 und 16 voll
   in die Wertung gingen.

Beides ist am 2026-08-13 behoben, zusammen mit der blockweisen Zentrierung, die
diesen Datensatz nachträglich auf σ 0,995 und ein sauberes Null bringt.
