# printcmp – algoritmer, testmaterial och mätningar

Hur analysen fungerar, vad testgeneratorn simulerar och vad som mättes under utvecklingen.
Algoritmerna utvecklades först i en Python-prototyp och portades sedan till C++-kärnan
(`core/libs`) med samma resultat (se `README.md`, avsnittet om pariteten). Siffrorna i
tabellerna nedan mättes med prototypen; C++-kärnan ger samma fynd på samma material.

| Del | C++ (core/libs/<bibliotek>/src) |
|---|---|
| JobSpec, toleransprofiler | `jobspec/jobspec.cpp`, `compare/profiles.cpp` |
| Dekorfärger och PDF-lager | `pdf/contentstream.cpp` |
| Rendering och sidobjekt (PDFium) | `pdf/pdfium.cpp`, `pdf/text.cpp` |
| Objektkarta, tabeller, punktskrift, masker | `artwork/objmap.cpp`, `artwork/structure.cpp`, `artwork/braille.cpp` |
| Hitta etiketter, passning, normalisering | `scan/locate.cpp`, `scan/register.cpp`, `scan/normalize.cpp` |
| Diff, veck, klassning, säkerhetsgrad | `scan/diff.cpp`, `scan/folds.cpp`, `scan/sheet_folds.cpp`, `scan/classify.cpp`, `scan/certainty.cpp`, `scan/aggregate.cpp` |
| Referensen och etikett/ark-jobbet | `compare/reference.cpp`, `compare/label.cpp`, `compare/label_steps.cpp`, `compare/textdiff.cpp` |
| Etiketterna mot varandra (utan original): detektering, mall, konsensus | `compare/peers.cpp` |
| Textlager, dokumentjämförelse, datakontroll | `doc/doctext.cpp`, `compare/doccompare.cpp`, `compare/datacheck.cpp` |
| Streckkoder | `barcode/barcode.cpp` (zxing-cpp) |
| Rapporter | `report/report.cpp`, `report/docreport.cpp` |
| Testmaterial: etikettmallar, PDF-skrivare, felkatalog | `testgen/templates.cpp`, `pdfwrite/writer.cpp`, `testgen/label.cpp`, `testgen/mutate.cpp` |
| Tryck- och scansimulering, testfall | `testgen/sim.cpp`, `testgen/scanner.cpp`, `testgen/make_case.cpp`, `testgen/doccases.cpp` |
| Utvärdering mot facit | `eval/scan_eval.cpp`, `eval/doc_eval.cpp` |

## Objektkategorier

Allt kategoriseras ur **PDF-originalet** (aldrig ur scannen). Varje skillnad får en
`category` och, där det är relevant, tabell- eller punktskriftsinformation.

| Kategori | Hur den hittas | Lägsta allvar |
|---|---|---|
| Text | Textlagret, tecken -> ord (Unicode) | Major |
| OutlinedText | ≥ 3 glyfstora vektorer (0,8–7 mm) på gemensam baslinje, tätt i rad | Major |
| TableCell | Skillnaden ligger i en identifierad tabell: rad, kolumn, radetikett, celltext | Major |
| TableRule | Skillnaden ligger på en tabellinje: "linjen mellan rad r och r+1" | Major |
| Barcode | Avkodning av renderat original; staplar/moduler märks som Barcode | Critical vid fel innehåll |
| Braille | Dekorfärg för prägling ("Braille", "Punktskrift", "Emboss" …) eller U+2800-tecken | Critical |
| Rule / Image / Vector | Tunna linjer utanför tabeller, bilder, övrig grafik | efter yta |

**Tabeller** (`artwork/structure.cpp`):

- *Med linjer:* vågräta linjer som delar x-intervall staplas till rader. Lodräta linjer,
  eller vita gångar mellan orden, ger kolumner (`ruled_grid` / `ruled_horizontal`).
- *Utan linjer:* textrader med jämnt radavstånd där en vit gång (≥ 1,5 mm och ≥ teckenhöjd)
  går genom alla rader, och första/sista raden har text på båda sidor (`unruled`).
  Löptext ger inga sådana gångar, så stycken blir inte tabeller.
- En flytt av en hel rad rapporteras som en "förskjuten rad", inte som flera fel.

**Punktskrift** (`artwork/braille.cpp`):

- Punkterna läses ur PDF:en, grupperas i rader och celler (punktavstånd och cellavstånd
  skattas) och översätts (svensk punktskrift, grundnivå: a–ö, siffror, versaltecken,
  komma, punkt, bindestreck).
- PDF mot PDF: punktmängderna jämförs cell för cell.
- PDF mot scan: alla sex lägen i varje cell mäts (reliefsignal = skuggning från präglingen).
  Tröskeln sätts per rad mellan präglade och tomma lägen. Ger saknade och extra punkter,
  väntad och läst bokstav, eller "prägling saknas" för hela raden.
- Punktskriftsområdet undantas från vanlig tryckjämförelse (präglingen är inget tryckfel).
- Celler under ett veck rapporteras som osäkra ("~" i läst text).

## Resultat (syntetisk testmängd, 20 ark / 160 etiketter, osedda frön)

Uppmätt med `tools/eval` på fall som genererades efter att utvecklingen var klar
(frön 2100, 2200, 2300, 2500, 2510). 600 dpi, 8 etiketter per ark, 2 CPU-kärnor.
"Riktade" fall använder näringsmallarna (tabell med och utan linjer, konturerad
rubrik, punktskrift, streckkod/QR) och riktade mutationer.

| Mätetal | Lätt (32) | Normal (64) | Svår (32) | Riktade (32) | Mål |
|---|---|---|---|---|---|
| Instanser hittade | 100 % | 100 % | 100 % | 100 % | 100 % |
| Artworkfel hittade (utom färg) | 40/40 | 112/112 | 16/16 | 96/96 | ≥ 99 % |
| Färgfel hittade (ΔE-tröskel 8) | – | – | 0/8 | – | – |
| Tryckfel hittade | 9/9 | 18/19 | 13/13 | 10/10 | ≥ 95 % |
| Rätt klass | 40/40 | 104/112 | 16/18 | 96/96 | ≥ 95 % |
| Rätt systematisk/lokal | 100 % | 100 % | 94 % | 100 % | ≥ 98 % |
| Falsklarm per etikett | 0,00 | 0,03 | 0,66 | 0,03 | ≤ 0,02 |
| Registreringsfel (RMS) | 0,03 px | 0,33 px | 0,37 px | 0,32 px | ≤ 0,5 px |
| Tid per ark (prototypen, 2 kärnor) | 63–82 s | 41–83 s | 61–86 s | 83–87 s | – |

**Per kategori och mutation (alla ark)**

| Kategori / mutation | Hittade | Rätt kategori |
|---|---|---|
| TableCell: ändrat cellvärde (t.ex. 6,6 g → 6,8 g) | 16/16 | 32/32 |
| TableCell: förskjuten rad | 16/16 | (ingår ovan) |
| TableRule: borttagen tabellinje | 8/8 | 8/8 |
| OutlinedText: ändrad bokstav i konturerad rubrik | 32/32 | 40/40 |
| Braille: ändrad punktskriftsbokstav | 40/40 | 47/47 |
| Braille: enstaka präglad punkt saknas | 7/7 | (ingår ovan) |
| Text och objekt: tecken, diakritiska tecken, ord, storlek, flytt, borttaget objekt | 144/144 | 112/112 |
| Streckkod: ändrad siffra | 8/8 | – |

**Slutsatser**

- Tabeller, konturerad text och punktskrift fungerar på simulerat tryck. Alla riktade
  fel hittades och fick rätt kategori, rad och kolumn eller linje.
- Falsklarmen är nära målet på lätta, normala och riktade ark (0–0,03 per etikett).
  På svåra ark ligger de på 0,66 per etikett. Nästan allt kommer från färgkalibreringen
  (unika, mättade kryddfärger; ett ark står för 16 av 21).
- Färgfel upptäcks inte i okalibrerat läge (0/8): självkalibreringen och ΔE 8 äter upp
  förskjutningar på 5–8 ΔE. Kalibrerad scanner (IT8) krävs för färgkontroll.
- Liten text (≤ 2,6 mm ordhöjd) har en känsligare tröskel. Annars missas ändringar som
  bara lägger till ett kort streck (6 → 8). Rubriker har den inte: rasterkanter mot färgad
  botten gav falsklarm.
- En flytt på ≈ 2 mm klassades i ett ark som "Form" i stället för "Flyttat". Felet
  hittades ändå.
- Siffrorna gäller simulerat tryck. Verkliga tryck och fysiskt vikta ark är nästa test.

## Simulator v2: flatbäddsscanner (CIS och CCD)

`testgen/scanner.cpp`, välj med `printcmp-testgen labels --scanner cis|ccd` (`legacy` = v1).

| Effekt | CIS (t.ex. Canon LiDE) | CCD (t.ex. Epson Perfection) |
|---|---|---|
| Oskärpa när papperet lyfter | 3–5 px per mm lyft (kort skärpedjup) | 0,3–0,7 px per mm |
| Ljusfall med höjd | snabbt (h0 1,2–1,8 mm) | långsamt (h0 6–10 mm) |
| Perspektiv tvärs vagnen | nej | ja: lyft innehåll förskjuts h·x/L (L 250–400 mm) |
| Kanalförskjutning R/G/B | ±0,25–0,4 px (dioderna blinkar i tur och ordning) | ±0,15 px (trilinjär sensor) |
| Präglingssignal | svagare | starkare (lägre ljusvinkel) |

Gemensamt: stegmotorns ojämnhet och skalfel i matningsriktningen, vinjettering,
signalberoende brus, färgprofil och tonkurva, damm på glaset (facit: `scan_artifacts`,
räknas varken som miss eller falsklarm).

Veck är en höjdkarta: bergveck (`h = lyft · e^(−|s|/w)`) eller dalveck (vecket mot glaset,
vingarna lyfter). Höjden ger oskärpa, ljusfall, sluttningsskuggning, förkortning och (CCD)
perspektiv. Bergveck kan spräcka färgen på heltäckande ytor. Stresstest: `--fold-p 1
--fold-lift 1.0`.

Ändringar i analysen som simulatorn ledde till:

- **Veckkandidater som återkommer** på samma etikettposition i många etiketter, utan att
  ligga på en linje över arket, är ändringar i originalet och inte veck.
- **Varje veckhypotes prövas mot alla etiketter** den korsar och behålls bara om den syns i
  de flesta.
- **Veck över tonplattor** godtas, inte bara över vitt papper.
- **Veckzonens bredd mäts** tvärs linjen (oskärpa och ljusfall), i stället för en fast bredd.
- **Stanslinjen eroderas även där den sammanfaller med sidkanten.**
- **Liten text** (≤ 2,6 mm) har en känsligare tröskel. En ändring som bara lägger till ett
  kort streck inne i ett ord (6 → 8, g → q) klassas som "Ändrat".

## Säkerhetsgrad: säkrast först

Varje skillnad får `certainty` = nivå, poäng och skäl (`scan/certainty.cpp`), och
rapporten grupperar efter nivå:

| Nivå | Belägg |
|---|---|
| Bekräftad (95–99) | Avkodat innehåll skiljer (streckkod), samma tydliga avvikelse på samma ställe i ≥ 75 % av etiketterna, eller PDF mot PDF |
| Mycket sannolik (80–90) | Tydlig signal (≥ 2,5 × tröskeln), utanför veck; punktskrift med god marginal |
| Trolig (60–75) | Över tröskeln men nära, eller nära ett veck |
| Möjlig (20–35) | Osäker zon (veck, registrering), svag signal, färg med okalibrerad scanner |

Ingen nivå är en garanti på 100 %. Poängen är än så länge en rangordning och inte en
kalibrerad sannolikhet; kalibrering mot simulatorns facit är ett naturligt nästa steg.

## PDF mot PDF: lagerval och textjämförelse

```json
{
  "schema": "printcmp.jobspec/1",
  "reference": {"path": "original.pdf"},
  "sample": {"path": "revision.pdf"},
  "layers": {"exclude": ["Ingredients EN"], "exclude_spots": ["White"]},
  "text_compare": true
}
```

- **Lager** (`pdf/contentstream.cpp`): PDF-lager (Optional Content, även OCMD och Form-XObject
  med `/OC`) och dekorfärger listas för båda filerna. `include` och `exclude` tar namn
  eller mönster. Uteslutna lager tas bort ur innehållsströmmen före rendering *och*
  textutvinning. Rapporten visar vilka lager som jämförts.
- **Text** (`compare/textdiff.cpp`): orden tas ur båda textlagren med position, typsnitt
  och storlek, i läsordning, och jämförs som sekvens. Resultatet är ändrat ord (med
  teckenvis skillnad, `1,[5→8]`), tillagt, borttaget, flyttat, eller samma ord med annan
  storlek eller annat typsnitt.
  - En grafisk skillnad som överlappar en textskillnad får texten i rapporten.
  - En textskillnad som inte syns grafiskt rapporteras som "Endast i textlagret", t.ex.
    vit text eller fel i ToUnicode-tabellen.

## Dokumentjämförelse: manus mot bok, versioner av en trycksak

För PDF:er vars layout skiljer sig (manus mot satt bok: annat sidformat, radbrytning,
avstavning, typsnitt, löpande huvud) går en bildjämförelse inte att använda.
Dokumentläget jämför i stället textlagret över alla sidor, med stil och läge.

```bash
printcmp-cli doc manus.pdf bok.pdf --out ut                              # omflödat (standard)
printcmp-cli doc v1.pdf v2.pdf --layout fixed --data förväntat.csv --out ut
```

eller i en JobSpec: `"compare": {"mode": "document", "layout": "reflow" | "fixed"}`.

| Del | Hur |
|---|---|
| Ord (`doc/doctext.cpp`) | Tecken för tecken ur PDFium: Unicode, ruta, baslinje, effektiv storlek (teckenstorlek × textmatris), typsnittsfamilj, fet, kursiv, färg. Ordgräns och radbyte mäts på tecknens stegbredd och baslinje, inte bläcket ("11,4" delas inte vid den smala ettan, ett citattecken hamnar inte på en egen rad). |
| Läsordning | Rader efter baslinje; spalter vid en lodrät lucka genom löptext (inte mellan tabellkolumner). |
| Sidmöbler | Korta rader överst/nederst som står fritt från brödtexten och antingen är ett sidnummer, upprepas på flera sidor eller är mindre än brödtexten (löpande huvud). De jämförs inte; sidnumreringen kontrolleras (ett fel tal, eller ett hopp). |
| Normalisering | Avstavning vid radslut lagas (behåller bindestrecket om ordet finns med bindestreck någon annanstans, "e-post"). Ligaturer, typografiska citattecken och streck normaliseras (`"typography": "strict"` stänger av). |
| Omflödat (`reflow`) | Ordföljden linjeras (difflib; för långa dokument först unika ankarord, patience). Ändrat/borttaget/tillagt per block med teckenvis skillnad; siffror = Critical. Flyttade stycken: borttaget på ett ställe och (nästan) samma text tillagt på ett annat; därefter längsta gemensamma block för stycken flyttade i bitar. |
| Fast layout (`fixed`) | Rader paras ihop efter innehåll var de än står, sedan ord för ord. En rad vars ankare (vänster-, höger- eller mittkant, och baslinje) flyttats är flyttad; ny storlek eller ny text räknas inte som flytt. |
| Stil | Omflödat: per *roll*. Manusets brödtext 12 pt får bli bokens 10 pt; det som avviker från hur rollen annars är satt rapporteras (en rubrik i fel storlek, en förlorad kursiv, ett stycke i fel typsnitt, ett ord i fel färg). En hel roll som bytt fetstil/kursiv rapporteras en gång. Fast layout: absolut. Rapporten visar rolltabellen (referensstil → provstil). |

## Datakontroll: förväntade uppgifter ur en textfil

Letar efter uppgifter ur en fil på etiketten eller i dokumentet. Ingen OCR: text söks i
textlagret, streckkoder avkodas.

```bash
printcmp-cli data etikett.pdf förväntat.txt              # slutkod 2 om något saknas
```

eller `"data_check": {"path": "förväntat.csv"}` i en JobSpec (etikett- eller dokumentläge).

```text
# förväntat.txt – fält: värde, eller bara värde; /…/ = reguljärt uttryck
Produkt: Ekologisk Havredryck
Volym: 1 liter
Batch: /LOT \d{4}-\d{4}/
EAN: 731086500123
```

- `.csv` (`,` `;` eller tabb) med en rad per variant: raden väljs med `"row"` (nummer eller
  `"kolumn=värde"`), annars den som stämmer bäst (summan av likheter).
- Utfall per uppgift: **finns** (sida, läge, antal), **nästan** (bästa kandidat och
  teckenvis skillnad, "2027-0[3→4]"; samma siffror med annan formatering noteras),
  **saknas**, **fel kod** (streckkoden finns men har annat innehåll).
- Streckkodsfält (EAN/GTIN/QR… eller rena sifferkoder) jämförs mot avkodade streckkoder;
  12 siffror godtas mot en EAN-13 med kontrollsiffra.
- Mot en scannad etikett: text mot det godkända originalets textlager, streckkoder mot
  scannen.

## Testmaterial och resultat: PDF mot PDF

`printcmp-testgen docs` genererar fall med facit, `printcmp-eval doc` mäter.

```bash
printcmp-testgen docs --out doccases --books 15 --labels 24 --clean 6 --seed 7000
printcmp-eval doc doccases --run --verbose --json doccases/metrics.json
```

- **Bokfall:** samma text satt som manus (A4, 12 pt, ojusterad, "Sida N", utkasthuvud) och
  som bok (A5, smal 10 pt, marginaljusterad, avstavad, ”…”, ﬁ-ligatur, löpande huvud,
  sidnummer i ytterhörnet) eller tvåspaltig tidningssida. Ändringar i boken: stavfel,
  siffra, skiljetecken, borttaget/tillagt ord, borttagen mening, flyttat stycke, förlorad
  kursiv/fetstil, rubrik i fel storlek, stycke i fel typsnitt, ord i fel färg, hopp i
  sidnumreringen. `--clean`: samma par utan ändringar.
- **Etikettfall:** två versioner av en etikettmall med ändrad text, storlek, typsnitt,
  fetstil, färg, flyttat element, två rader som bytt plats, borttaget/tillagt ord, ändrad
  EAN, plus `expected.txt` eller `expected.csv` (tre varianter) där vissa värden avsiktligt
  är nästan rätt, saknas eller har fel streckkod.

Resultat på en ny, orörd uppsättning (frö 7000; inget ändrat efter att den genererats):

| | Fall | Ändringar hittade | Falsklarm | Data |
|---|---|---|---|---|
| Bok/tidning | 21 (varav 6 utan ändringar) | 132/132 | 0 | – |
| Etiketter | 24 | 66/66 | 0 | 150/150 uppgifter, 24/24 rätt variant |

Under utvecklingen rättades bland annat ordgränser (stegbredd i stället för bläck), radbyten
vid citattecken, spaltdetektering, sidmöbler (löpande huvud med kapitelnamn, sidnummer i
vänsterspalten), flyttade stycken som börjar med samma ord som texten de hamnar framför,
och radparning i fast layout. En 71-sidig manus mot en 62-sidig bok (15 000 ord) tar
2,7 s att läsa och 0,1 s att jämföra.

**Begränsningar:** testmaterialet är genererat av samma verktyg (DejaVu-typsnitt, regelbunden
struktur). Verkliga PDF:er från InDesign och Word har kerning utan mellanslag, fotnoter,
marginalanteckningar, bilder med text, rubriker över flera spalter och taggad/otaggad text
– de behöver provas. Fast layout jämför sidor med samma index. Rader som flyttats till en
annan sida i fast layout paras bara inom samma sida.

## Resultat v2: simulerad flatbäddsscanner

Utvecklingen gjordes mot CIS-fall. CCD-fallen genererades samtidigt men kördes först när
koden var fryst (undanhållna, ingen justering efteråt). 600 dpi, 8 etiketter per ark.

| | CIS normal | CIS svår | CIS riktade | CCD normal | CCD svår | CCD riktade |
|---|---|---|---|---|---|---|
| Ark / etiketter | 6 / 48 | 4 / 32 | 2 / 16 | 6 / 48 | 4 / 32 | 2 / 16 |
| Originalfel hittade (utom färg) | 80/80 | 72/72 | 48/48 | 56/56 | 48/48 | 48/48 |
| Färgfel hittade | – | 0/16 | – | – | 0/16 | – |
| Tryckfel hittade | 10/11 | 7/9 | 3/3 | 10/10 | 7/7 | 5/5 |
| Falsklarm per etikett | 0,00 | 0,81 | 0,00 | 0,33 | 1,59 | 0,00 |

Falsklarmen har två huvudorsaker:

- **Trycksimulatorn**, inte analysen: på kryddmallen trycks en liten mättad grön form
  mycket mörkare än originalet. Det ger samma systematiska avvikelse på alla etiketter
  (16 av CIS svårs 26, alla 16 i CCD normal).
- **Ett CCD-ark med ett dalveck som lyfter 1 mm** (51 av CCD svårs 51). Perspektivet
  flyttar lyft innehåll i sidled och veckzonen blev för smal.

**Stresstest: vecklyft** (2 ark per nivå och scanner, ett veck över arket)

| Lyft | CIS: osäker zon | CIS: falsklarm/etikett | CCD: osäker zon | CCD: falsklarm/etikett |
|---|---|---|---|---|
| 0,2 mm | 3 mm | 0,00 | 3 mm | 0,00 |
| 0,5 mm | 8 mm | 0,13 | 6 mm | 0,00 |
| 1,0 mm | 25 mm | 0,19 | 4 mm | 0,38 |
| 1,5 mm | 25 mm | 0,00 | 23 mm | 0,00 |

Alla originalfel hittades på alla nivåer.

- **CIS:** ett veck som lyfter ≥ 1 mm gör ungefär halva etiketthöjden obedömbar.
- **CCD:** klarar ~1 mm med en smal zon. Vid 1,5 mm är båda lika dåliga.
- **Rekommendation:** CCD-scanner när arken kan vara vikta; med CIS, pressa arket plant
  (lock och tyngd) så att lyftet blir under ~0,5 mm.
- Underlaget är litet (2 ark per nivå) och ska ses som en riktning, inte som exakta gränser.

## Fast benchmark och veckförbättringar (oktober 2026)

`printcmp-eval bench` genererar samma ark med fasta frön och utvärderar dem, så att
ändringar kan jämföras före och efter. CIS och CCD använder samma frön; mallarna roteras.

```bash
printcmp-eval bench --out bench --jobs 8                 # alla uppsättningar
printcmp-eval bench --out bench --sets ccd_hard          # en uppsättning
```

| Uppsättning | Svårighet | Ark | Frön | Särskilt |
|---|---|---|---|---|
| cis_normal / ccd_normal | normal | 6 | 5000–5005 | |
| cis_hard / ccd_hard | svår | 4 | 5100–5103 | |
| cis_fold05 | normal | 4 | 5200–5203 | ett veck, lyft 0,5 mm |
| ccd_fold10 | normal | 4 | 5200–5203 | ett veck, lyft 1,0 mm |
| ccd_fold15 | normal | 4 | 5300–5303 | ett veck, lyft 1,5 mm |

Ändringar:

- **Trycksimulatorn gjorde mättade färger för mörka.** Korrektionen mot PDF:ens färg var
  multiplikativ, och i heltäckande raster är 60–90 % av pixlarna i någon kanal exakt 0 –
  de går inte att lyfta genom multiplikation. Kryddmallens stjälk hamnade på L* 34 i
  stället för 53 (ΔE 19–21 mot 5–10 för stora ytor). Nu läggs lågpassresten till additivt
  efteråt; alla former ligger på ΔE 4–8, en jämn punktförstoring som färgavbildningen tar.
- **Veckzonens bredd följer förskjutning** (`scan/folds.cpp`, `estimate_halfwidth`). Ett CCD-veck flyttar
  lyft innehåll i sidled utan synlig oskärpa. Förskjutningen skattas med minsta kvadrat ur
  referensens gradient i bitar om 10 mm längs linjen och zonen fortsätter så länge den är
  > 1 px och > 2,5 × nivån långt från vecket. En bit räknas bara om förskjutningen förklarar
  ≥ 40 % av resten: en strimma parallellt med vecket ger annars en skenbar förskjutning
  som vidgar zonen och döljer strimman.
- **Svaga veck ur fynd på en linje** (`compare/label.cpp`, `collinear_folds`). Ett lågt bergveck på CCD
  syns knappt, men färgen spricker på heltäckande ytor. Smala fynd i ≥ 3 etiketter på samma
  linje i arket, på olika ställen i etiketterna och i minst hälften av etiketterna linjen
  korsar, blir en osäker veckzon. Fynd som återkommer på samma ställe i etiketterna
  (originalfel, t.ex. kanterna på en flyttad streckkod i en kolumn) bär aldrig en linje.

Resultat (före · simulatorfix · simulatorfix och veckändringar). Originalfel utom färg;
färgfelen (0/8 i tre uppsättningar) är oförändrade.

| Uppsättning | Etiketter | Originalfel | Tryckfel | Falsklarm/etikett |
|---|---|---|---|---|
| cis_normal | 48 | 48/48 · 48/48 · 48/48 | 7/7 · 7/7 · 7/7 | 0,00 · 0,00 · 0,00 |
| cis_hard | 32 | 40/40 · 40/40 · 40/40 | 6/7 · 6/7 · 6/7 | 0,16 · 0,16 · 0,16 |
| ccd_normal | 48 | 48/48 · 48/48 · 48/48 | 7/7 · 7/7 · 7/7 | 0,00 · 0,00 · 0,00 |
| ccd_hard | 32 | 40/40 · 40/40 · 40/40 | 7/7 · 7/7 · 7/7 | 0,38 · 0,19 · **0,00** |
| cis_fold05 | 32 | 24/24 · 24/24 · 24/24 | 9/9 · 9/9 · 9/9 | 0,00 · 0,00 · 0,00 |
| ccd_fold10 | 32 | 24/24 · 24/24 · 24/24 | 9/9 · 9/9 · 9/9 | 0,19 · 0,28 · 0,19 |
| ccd_fold15 | 32 | 56/56 · 56/56 · 56/56 | 7/8 · 6/8 · 6/8 | 1,03 · 0,44 · 0,41 |

Simulatorfixen ändrar arken (samma frön, andra pixlar), så kolumn 1 och 2 jämför simulatorer
och kolumn 2 och 3 jämför analysen på identiska ark. Kvar: punktskrift som läses fel nära
veck (5 etiketter på ett CIS-ark), och fynd vid etikettkanten när ett CCD-veck lyfter 1,5 mm.
Zonen når där sitt tak på 12 mm. Som tidigare: litet underlag, simulerat tryck.

## Lärdomar under bygget (viktiga för C++-kärnan)

- **OpenCV 5 `phaseCorrelate` skriver fönstret in i indata på plats.** Med delade vyer
  förstörs referensbilden för nästa instans. Skicka alltid kopior (rättat i `scan/register.cpp`).
- **Optiskt flöde kräver ljushetskonstans.** Papper, scannerfärg och raster bryter den;
  lokal kontrastnormalisering med stort epsilon behövs, och flödet måste verifieras per
  område (behålls bara där det minskar felet), annars "förklarar" det bort riktiga fel.
- **Färgkalibrering mot scannen ska inte låtas dominera av papperet.** Robusta vikter med
  en golvnivå (≈ ΔE 5) och ett svärteankare krävs; annars mappas svart fel.
- **Skärpematchning:** scannern är oskarpare än renderingen; tunna streck (streckkoder)
  tappar kontrast och tolkas som saknade om referensen inte suddas lika mycket.
- **Veckband** utvärderas inte – där rapporteras en osäker zon. Systematiska fel som
  hittas i övriga instanser förs ändå över till instanser där området var osäkert.
- **Veck hanteras på arknivå:** ett veck som syns i en instans projiceras till alla
  instanser det korsar och finjusteras mot varje instans residual.
- **Affin global modell, inte homografi:** en homografi anpassad till ett veckat ark ger
  falska perspektivtermer och fel längs etikettkanterna.
- **Saknat/tillagt avgörs mot lokal bakgrund**, inte ljushetens tecken (vitt element på
  färgad botten).
- **Mät förskjutning mot vad diffen tål, inte mot vad som går att mäta.** Kring varje veck
  finns 0,5–1 px verklig förkortning som diffen redan tål; en lägre gräns vidgar bara zonen
  och döljer tryckfel nära vecket.
- **En skattning ur minsta kvadrat ska förklara resten.** Ett tryckfel ger också en
  "förskjutning"; kräv att modellen förklarar en rimlig andel av signalen (R²).
- **Konsensuspasset hittar de svaga fynden.** Arknivåkontroller av fynd (linjer, upprepning)
  måste köras efter det, inte före.

## Kända begränsningar

- Färgkontroll i okalibrerat läge (självkalibrering per jobb): ΔE-tröskel 8. Små,
  mättade element med unika färger kalibreras sämst. Med IT8-kalibrerad scanner bör
  3–5 vara möjligt.
- Globala färgfel som täcker större delen av etiketten kalibreras bort av självkalibreringen.
- Overprint simuleras inte (PDFium). Form-XObjects tolkas inte av `pdfspots`.
- Streckkoder: innehåll jämförs, gradering (ISO 15416/15415) ingår inte. DataMatrix saknas.
- Punktskrift: punkthöjd mäts inte (kräver 3D-mätning). Översättningen är grundnivå; för
  andra språk och förkortad punktskrift behövs liblouis. Läget för Unicode-punktskrift i
  textlagret är en approximation ur teckenrutan.
- Tabeller i flera nivåer (sammanslagna celler, rubriker över flera kolumner) delas upp i
  enkla rader/kolumner. Text i tabellceller jämförs grafiskt, inte som värden.
- Konturerad text känns igen geometriskt; en rad med ≥ 3 likstora ikoner på samma baslinje
  kan tolkas som text.
- Simulatorn är en förenkling; verkliga tryck (flexo/offset) och fysiska veck behövs för
  att mäta falsklarm på riktigt. Kända brister i v2:
  - Präglingen är perfekta kupoler.
  - Papper saknar fiberstruktur och genomlysning.
  - Ett veck är alltid en rak linje över hela arket.
- Veck som lyfter ~1,5 mm på CCD ger fortfarande falsklarm (0,4 per etikett), främst vid
  etikettkanten; veckzonen når sitt tak på 12 mm. Punktskrift nära veck kan läsas fel.
- Textjämförelsen kräver ett textlager. Konturerad text och text i bilder jämförs bara grafiskt.
  Läsordningen är heuristisk (rader och kolumner); kraftigt omflyttad layout ger fler
  textskillnader än nödvändigt.
