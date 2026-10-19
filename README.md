# printcmp – prototyp (spik)

Grafisk jämförelse av tryckta/scannade etiketter mot PDF-original, utan OCR.
Syftet är att på några veckor visa om det svåra – registrering, veck och falsklarm –
går att lösa tillräckligt bra, innan native-kärnan byggs.

Modulerna speglar den planerade C++-kärnan, så att varje del kan ersättas en och en.

```
printcmp/
  jobspec.py     JobSpec-schema, standardvärden, toleransprofiler
  pdfspots.py    dekorfärger som inte trycks (punktskrift, stanslinje, lack) + sanerad PDF
  render.py      PDF-rendering (PDFium via pypdfium2) bakom ett utbytbart gränssnitt
  objmap.py      objektkarta: Text (Unicode ur PDF:en), OutlinedText, Rule, Barcode, Image, Vector
  structure.py   tabeller med rutnät, med vågräta linjer eller utan linjer -> rader/kolumner/celler
  braille.py     punktskrift: punkter ur PDF, cellnät, svensk översättning, präglingskontroll
  mask.py        masker: utanför stanslinje, variabel data, lack/folie
  instances.py   hitta alla etiketter på arket (rotation 0/90/180/270 + skevhet),
                 rutnätsordning, aggregering till systematiska/lokala fel
  register.py    global homografi (faskorrelation i tiles + RANSAC), lokal deformation
                 (DIS-flöde regulariserat till nät, verifierat per område), en omsampling
  normalize.py   rasterdämpning, skärpematchning (MTF), robust färgavbildning
  folds.py       veckdetektion (långa raka residualband) -> osäkra zoner
  diff.py        struktur (L* mot grannskapets min/max + SSIM) och färg (CIEDE2000)
  classify.py    regler: Saknas/Tillagt/Ändrat/Flyttat/Färg/Form/Streckkod/Osäker + kategori,
                 tabellcell/-linje och lägsta allvarlighet per kategori
  barcode.py     avkodning och innehållsjämförelse (OpenCV: EAN/UPC/Code128, QR)
  pipeline.py    orkestrering: JobSpec in -> result.json + bildlager + report.html
  report.py      fristående HTML-rapport (PNG-miniatyrer som data-URI)
tools/
  testgen/       testfallsgenerator: etikettmallar -> PDF, felkatalog (mutationer på
                 beskrivningsnivå), tryck-/scansimulering (raster, punktförstoring,
                 passningsfel, utskjutning, rotation, veck, scanner-MTF, brus), facit
  eval/          utvärdering mot facit: träffsäkerhet, falsklarm, klass, registrering
tests/           snabba tester (python -m unittest discover -s tests -v)
```

## Kom igång

Python 3.11+ och: `numpy scipy opencv-contrib-python scikit-image pypdfium2 pikepdf
reportlab tifffile jsonschema`.

```bash
# 1. Generera testfall (referens-PDF, scannat ark, JobSpec, facit)
python -m tools.testgen.make_cases --out cases --n 6 --seed 300 --difficulty normal

# 2. Kör ett jobb
python -m printcmp run cases/case_00300/jobspec.json
#    -> cases/case_00300/out/result.json, report.html, layers/

# 3. Kör alla fall och mät mot facit
python -m tools.eval.evaluate cases --run --json metrics.json --verbose
```

Eget material: skriv en JobSpec som pekar på er PDF och en scannad TIFF (600 dpi,
okomprimerad, ingen auto-förbättring i scannern):

```json
{
  "schema": "printcmp.jobspec/1",
  "reference": { "path": "etikett.pdf" },
  "sample": { "path": "ark.tif", "kind": "scan" },
  "instances": { "mode": "multi", "expected_layout": { "rows": 4, "cols": 2 } },
  "masks": [ { "reason": "variable", "rect_mm": [3.5, 34.6, 22, 3.6] } ],
  "tolerance_profile": "label_standard"
}
```

PDF mot PDF: sätt `sample.path` till en PDF; profilen `pdf_vs_pdf` väljs automatiskt.

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

**Tabeller** (`structure.py`):

- *Med linjer:* vågräta linjer som delar x-intervall staplas till rader. Lodräta linjer,
  eller vita gångar mellan orden, ger kolumner (`ruled_grid` / `ruled_horizontal`).
- *Utan linjer:* textrader med jämnt radavstånd där en vit gång (≥ 1,5 mm och ≥ teckenhöjd)
  går genom alla rader, och första/sista raden har text på båda sidor (`unruled`).
  Löptext ger inga sådana gångar, så stycken blir inte tabeller.
- En flytt av en hel rad rapporteras som en "förskjuten rad", inte som flera fel.

**Punktskrift** (`braille.py`):

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
| Tid per ark (Python, 2 kärnor) | 63–82 s | 41–83 s | 61–86 s | 83–87 s | – |

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

`tools/testgen/scanner.py`, välj med `make_cases --scanner cis|ccd` (`legacy` = v1).

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

Varje skillnad får `certainty` = nivå, poäng och skäl (`printcmp/certainty.py`), och
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

- **Lager** (`printcmp/layers.py`): PDF-lager (Optional Content, även OCMD och Form-XObject
  med `/OC`) och dekorfärger listas för båda filerna. `include` och `exclude` tar namn
  eller mönster. Uteslutna lager tas bort ur innehållsströmmen före rendering *och*
  textutvinning. Rapporten visar vilka lager som jämförts.
- **Text** (`printcmp/textdiff.py`): orden tas ur båda textlagren med position, typsnitt
  och storlek, i läsordning, och jämförs som sekvens. Resultatet är ändrat ord (med
  teckenvis skillnad, `1,[5→8]`), tillagt, borttaget, flyttat, eller samma ord med annan
  storlek eller annat typsnitt.
  - En grafisk skillnad som överlappar en textskillnad får texten i rapporten.
  - En textskillnad som inte syns grafiskt rapporteras som "Endast i textlagret", t.ex.
    vit text eller fel i ToUnicode-tabellen.

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

## Avvikelser från planen i den här miljön

PyPI var inte nåbart, så följande ersättningar gjordes. Gränssnitten är förberedda för bytet.

| Planerat | I prototypen | Byte |
|---|---|---|
| MuPDF (rendering, overprint) | PDFium (pypdfium2) | ny klass i `render.py` |
| zxing-cpp (inkl. DataMatrix) | OpenCV barcode/QR | `barcode.detect()` |
| Elastix (B-spline) | DIS-flöde + nät + verifiering | `register.local_flow()` |
| colour-science | scikit-image (CIEDE2000) | – |

## Lärdomar under bygget (viktiga för C++-kärnan)

- **OpenCV 5 `phaseCorrelate` skriver fönstret in i indata på plats.** Med delade vyer
  förstörs referensbilden för nästa instans. Skicka alltid kopior (rättat i `register.py`).
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
  - Trycksimuleringen gör små mättade objekt för mörka.
  - Präglingen är perfekta kupoler.
  - Papper saknar fiberstruktur och genomlysning.
  - Ett veck är alltid en rak linje över hela arket.
- Veck som lyfter mer än ~1 mm på CCD kan ge falsklarm intill veckzonen (perspektivförskjutning).
- Textjämförelsen kräver ett textlager. Konturerad text och text i bilder jämförs bara grafiskt.
  Läsordningen är heuristisk (rader och kolumner); kraftigt omflyttad layout ger fler
  textskillnader än nödvändigt.
