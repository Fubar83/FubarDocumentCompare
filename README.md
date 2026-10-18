# printcmp – prototyp (spik)

Grafisk jämförelse av tryckta/scannade etiketter mot PDF-original, utan OCR.
Syftet är att på några veckor visa om det svåra – registrering, veck och falsklarm –
går att lösa tillräckligt bra, innan native-kärnan byggs.

Modulerna speglar den planerade C++-kärnan, så att varje del kan ersättas en och en.

```
printcmp/
  jobspec.py     JobSpec-schema, standardvärden, toleransprofiler
  pdfspots.py    dekorfärger som inte trycks (stanslinje, lack) + sanerad PDF för rendering
  render.py      PDF-rendering (PDFium via pypdfium2) bakom ett utbytbart gränssnitt
  objmap.py      objektkarta: text (Unicode från PDF:en), bild, vektor
  mask.py        masker: utanför stanslinje, variabel data, lack/folie
  instances.py   hitta alla etiketter på arket (rotation 0/90/180/270 + skevhet),
                 rutnätsordning, aggregering till systematiska/lokala fel
  register.py    global homografi (faskorrelation i tiles + RANSAC), lokal deformation
                 (DIS-flöde regulariserat till nät, verifierat per område), en omsampling
  normalize.py   rasterdämpning, skärpematchning (MTF), robust färgavbildning
  folds.py       veckdetektion (långa raka residualband) -> osäkra zoner
  diff.py        struktur (L* mot grannskapets min/max + SSIM) och färg (CIEDE2000)
  classify.py    regler: Saknas/Tillagt/Ändrat/Flyttat/Färg/Form/Streckkod/Osäker
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

## Resultat (syntetisk testmängd, 22 ark / 176 etiketter, osedda frön)

Uppmätt med `tools/eval` på fall som genererades efter att utvecklingen var klar
(frön 700–703, 800–811, 900–905). 600 dpi, 8 etiketter per ark, 2 CPU-kärnor.

| Mätetal | Lätt (32) | Normal (96) | Svår (48) | Mål |
|---|---|---|---|---|
| Instanser hittade | 100 % | 100 % | 100 % | 100 % |
| Artworkfel hittade (utom färg) | 16/16 | 96/96 | 112/112 | ≥ 99 % |
| Färgfel hittade (ΔE-tröskel 8) | 0/8 | 0/16 | – | – |
| Tryckfel hittade | 6/6 | 28/32 | 13/14 | ≥ 95 % |
| Rätt klass | 16/16 | 88/96 | 104/112 | ≥ 95 % |
| Rätt systematisk/lokal | 100 % | 100 % | 100 % | ≥ 98 % |
| Falsklarm per etikett | 0,00 | 0,16 | 0,40 | ≤ 0,02 |
| Registreringsfel (RMS, ej veckade) | 0,04 px | 0,38 px | 0,93 px | ≤ 0,5 px |
| Tid per ark (Python, 2 kärnor) | 35–50 s | 30–70 s | 32–51 s | – |

**Slutsatser**

- Registrering, lokalisering (även blandade 0/180°) och klassning av artworkfel fungerar.
- Falsklarmen ligger 8–20 gånger över målet. De återstående är "Flyttat"/"Form" nära
  veck och små kantavvikelser – nästa steg är bättre lokal registrering (B-spline,
  Elastix-klass) och toleranser per objekttyp.
- Färgfel upptäcks inte i okalibrerat läge: självkalibreringen och tröskeln ΔE 8 gör att
  förskjutningar på 5–8 ΔE försvinner. Kalibrerad scanner (IT8) krävs för färgkontroll.
- Små, ljusa fläckar (< 0,3 mm) missas ibland.
- Siffrorna gäller simulerat tryck. Verkliga tryck och fysiskt vikta ark är nästa test.

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
- Simulatorn är en förenkling; verkliga tryck (flexo/offset) och fysiska veck behövs för
  att mäta falsklarm på riktigt.
