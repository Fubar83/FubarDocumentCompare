# printcmp

Jämför tryck och scannade ark mot PDF-original, och PDF mot PDF (manus mot bok, versioner av
en etikett, förväntade uppgifter ur en textfil). Ingen OCR: allt som ska läsas tas ur PDF:ens
textlager eller avkodas (streckkoder).

```
core/              C++20-kärnan: lagrade bibliotek, C-API (FubarDocumentCompare.Native.dll) och printcmp-cli
bindings/dotnet/   .NET-biblioteket FubarDocumentCompare med xUnit-tester; FubarDocumentCompare.slnx bygger allt i .NET
examples/dotnet/   körbara C#-exempel på vanliga uppgifter (examples/dotnet/README.md)
demo/              demoapp i Avalonia (Windows/macOS/Linux): jämförelser, demoark, PDF-vyn (demo/README.md)
testdata/          små genererade fall med facit, för testerna
docs/              algoritmer, testmaterial och mätningar (docs/algoritmer.md)
```

## Arkitektur

```
core/
  libs/<namn>/                ett statiskt bibliotek per ansvarsområde (pc_<namn>)
    include/printcmp/<namn>/  publikt gränssnitt – det enda andra bibliotek kan inkludera
    src/                      implementation och privata rubriker
    CMakeLists.txt            PUBLIC- och PRIVATE-beroenden
  capi/                       FubarDocumentCompare.Native.dll: det stabila C-API:t (include/fdc/fdc.h, fdc_*)
  apps/                       printcmp-cli, printcmp-testgen, printcmp-eval, printcmp-img (tunna skal)
  tests/                      ctest: core, capi, smoke, architecture, imgproc, docview, fonts, steps; realworld (valfritt)
```

| Lager | Bibliotek | Ansvar | Beroenden |
|---|---|---|---|
| Grund | `pc_core` | typer, fel, jobb (händelser, avbrott), numerik, filer, Unicode, regex, difflib | nlohmann-json; privat utf8proc, PCRE2 |
| Tjänster | `pc_image` | bildinläsning (TIFF med dpi), CIELAB, CIEDE2000, SSIM | OpenCV; privat libtiff |
| | `pc_imgproc` | bildbehandling på områden: filter, FFT, mönstersökning, mätning, ritning | OpenCV |
| | `pc_pdf` | läsa PDF: dokument, rendering (även utsnitt), sidfakta och typsnitt, tillhandahållna typsnitt (fontfiler, uppsättningar, breddkontroll), sidobjekt, textlager per tecken, dekorfärger, lager | core; privat PDFium, qpdf |
| | `pc_pdfwrite` | skriva PDF: TrueType-delmängder, CMYK och dekorfärger, lager | privat core, zlib |
| | `pc_braille` | svensk punktskrift: text ↔ celler | privat core |
| | `pc_barcode` | avkoda och koda streckkoder och QR | core; privat zxing-cpp |
| | `pc_docview` | tillståndslösa PDF-tjänster för visning: metadata, varianter (lager/dekorfärger), rendering i rutor, överlägg; innehållsadresserad cache | pdf; privat barcode, image |
| Analys | `pc_doc` | textlager över alla sidor: ord, stil, läsordning, sidmöbler, avstavning | pdf |
| | `pc_artwork` | originalets objektkarta, tabeller, punktskrift, masker | pdf, braille |
| | `pc_scan` | etiketter på arket, passning, normalisering, diff, veck (även på arknivå), klassning, säkerhetsgrad | artwork, barcode; privat image |
| | `pc_compare` | jobben: referens, etikett/ark, dokument, datakontroll, toleransprofiler | scan, doc |
| Tillämpning | `pc_report` | export: result.json, HTML-rapporter, bildlager | compare |
| | `pc_jobspec` | JobSpec: tolkning (`jobspec.hpp`) och körning (`runner.hpp`) | compare; privat report |
| | `FubarDocumentCompare.Native.dll` | C-API (`fdc_*`) som fasad över compare, report och docview | privat compare, report, docview |
| Testsviter | `pc_testgen` | testmaterial: mallar, felkatalog, tryck- och scansimulering, dokumentfall | pdfwrite; privat braille, barcode, pdf, image |
| | `pc_eval` | mätetal mot facit | privat doc |
| | `pc_bench` | köra sviter och den fasta benchmarken | privat testgen, jobspec, eval |

Principer:

- **Isolering:** ett bibliotek ser bara de publika rubrikerna hos biblioteken det länkar.
  - PDFium och qpdf används bara i `pc_pdf`, zxing-cpp bara i `pc_barcode`, zlib bara i
    `pc_pdfwrite` och libtiff bara i `pc_image`.
  - Testet `architecture` (`core/cmake/check_architecture.cmake`) kontrollerar det, och att
    publika rubriker bara inkluderar PUBLIC-beroenden.
- **En modul per ansvar:** varje bibliotek har en rubrik per modul (t.ex.
  `printcmp/scan/register.hpp`) och implementationen i en fil per modul.
  - Samlingsrubrikerna (`printcmp/scan/scan.hpp` osv.) finns för program.
  - Biblioteken själva inkluderar bara modulerna de använder.
- **Tolkning skild från körning och export:**
  - En JobSpec tolkas till en typad `JobSpec`.
  - `jobspec::run` kör den.
  - `pc_report` skriver bara utdata.
- **Tunna program:** kommandoradsprogrammen tolkar argument; logiken finns i biblioteken.
- **Stabil yta utåt:** C-API:t är det enda som exporteras. .NET-biblioteket använder bara det.

PDFium skyddas av ett globalt lås. Allt annat är trådsäkert, och flera jobb körs parallellt
(motorn begränsar antalet).

### API-modellen

Data och tjänster är åtskilda. Tjänsterna tar data som argument och returnerar data; de håller
inget tillstånd mellan anropen.

```
DocumentCompareEngine (kompositionsrot: inbyggd motor, jobbgräns, innehållsadresserad PDF-cache)
  ├─ PdfInspector   metadata (sidor, rutor, lager, dekorfärger, typsnitt, varningar), överlägg
  ├─ PdfRenderer    sidor, utsnitt och rutor (rutpyramid), varianter av lager och dekorfärger
  ├─ ImageCodec     PNG/JPEG, pixlar
  └─ Comparer ──► Reference (originalet, analyseras en gång, oföränderligt, delas mellan trådar)
              └─► Job (asynkront: händelser, avbrott) ──► Result (fynd, etiketter, datafält, utsnitt, export)
```

- **Tillståndslösa PDF-tjänster.** En PDF anges som `PdfSource`: SHA-256 plus ett sätt att hämta
  innehållet (fil, minne, S3 …).
  - Motorns cache (nyckel: SHA-256) gör att innehållet bara hämtas och tolkas första gången.
  - Innehållet kontrolleras mot den angivna SHA-256.
  - Cachen påverkar bara hastigheten, aldrig resultaten, och kan tömmas när som helst.
- **Koordinater** i överlägg: mm från vald rutas (standard trim) övre vänstra hörn, y nedåt.
  Samma ruta som renderingen, så `px = mm · dpi / 25,4`.
- **Rutpyramid:** nivå L = 18 · 2^L dpi (0–8).
  - Rutorna är pixellika med en rendering av hela sidan.
  - PDFium ritar tecken som korsar bitmappens kant annorlunda. Därför renderas varje utsnitt med
    en marginal som rymmer sidans största tecken, och marginalen skärs sedan bort.
- **Inspektera, konfigurera, kör.** Allt som kräver ett beslut (lager, dekorfärger,
  streckkoder i originalet) läses från referensen innan jobbet startar. Ett jobb ställer
  aldrig frågor medan det körs.
- **Händelser** under körningen: förlopp per steg, arket inläst, etikett hittad (med hörn),
  veck hittat, etikett analyserad (preliminär status), varning, klart/fel/avbrutet. De
  levereras som återanrop på en arbetstråd, eller hämtas med `fdc_job_poll`.
- **Fynd** har typ, allvar, säkerhetsgrad (bekräftad, hög, trolig, möjlig) med skäl, läge i
  mm, text före/efter och ett stabilt *fingeravtryck* (för "godkänn och ignorera framöver").
- Varje struct börjar med `size`, så att fält kan läggas till utan att bryta ABI:t.

C:

```c
fdc_engine* e;           fdc_engine_create(NULL, &e);
fdc_pdf* pdf;            fdc_pdf_load(e, bytes, len, &pdf);            /* senare: fdc_pdf_find(e, sha256, &pdf) */
fdc_buffer* meta;        fdc_pdf_describe(e, pdf, &meta);              /* JSON */
fdc_render_options o;    fdc_render_options_init(&o); o.dpi = 150; o.exclude_layers = "Mått";
fdc_image* img;          fdc_pdf_render(e, pdf, &o, &img);
fdc_buffer* png;         fdc_image_encode(img, FDC_FORMAT_PNG, -1, &png);

fdc_reference* ref;      fdc_reference_open(e, "original.pdf", NULL, &ref);
fdc_source s;            fdc_source_init(&s); s.kind = FDC_SOURCE_SCAN_FILE; s.path = "ark.tif";
fdc_job* job;            fdc_job_start_compare(e, ref, &s, NULL, on_event, ctx, &job);
fdc_result* res;         fdc_job_wait(job, -1, &res);
```

C#:

```csharp
using var engine = new DocumentCompareEngine(new EngineOptions { CacheBytes = 2L << 30 });

var pdf = new PdfSource(sha256, ct => s3.OpenAsync(key, ct));      // eller PdfSource.FromFile / FromBytes
var meta = await engine.PdfInspector.GetMetadataAsync(pdf);
using var page = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions
{
    Dpi = 150,
    Variant = new PdfVariant { ExcludeLayers = ["Mått"], HideNonPrinting = true },
});
byte[] png = engine.ImageCodec.Encode(page, ImageFormat.Png);
using var tile = await engine.PdfRenderer.RenderTileAsync(pdf, new TileSpec(Level: 5, X: 3, Y: 2));
var map = await engine.PdfInspector.GetOverlaysAsync(pdf, new OverlayOptions { Kinds = OverlayKinds.All });

using var reference = engine.Comparer.OpenReference("original.pdf", new ReferenceOptions { ExcludeSpots = ["White"] });
using var job = engine.Comparer.StartCompare(reference, Sample.ScanFile("ark.tif"), new CompareOptions { Rows = 4, Cols = 2 });
await foreach (var e in job.Events.ReadAllAsync())          // eller job.EventReceived, eller IProgress<JobEvent>
    if (e.Type == EventType.LabelLocated) Console.WriteLine($"etikett {e.Index}");
using var result = await job.Completion;
byte[] json = engine.ResultExporter.ToJson(result);                 // även ToReportHtml, CropPng(result, i, Side.Sample)
```

Fler körbara exempel finns i `examples/dotnet` (rendering, varianter, rutor, överlägg, typsnitt, lager,
jämförelser och kö).

Namnrymder: `FubarDocumentCompare` (motor, fel, inställningar), `.Pdf`, `.Imaging`, `.Fonts` och
`.Comparison`.

Den inbyggda DLL:en heter `FubarDocumentCompare.Native.dll` och .NET-sammansättningen
`FubarDocumentCompare.dll`.

## Bygga

Verktyg (winget):

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

```bash
winget install Kitware.CMake Git.Git Microsoft.DotNet.SDK.8
```

```bash
git clone https://github.com/microsoft/vcpkg $HOME/vcpkg && $HOME/vcpkg/bootstrap-vcpkg.bat
```

Kärnan (PowerShell; `dev.ps1` laddar MSVC, vcpkg, CMake och Ninja i sessionen). Första
bygget hämtar och bygger beroendena via vcpkg (manifest i `core/vcpkg.json`) och PDFium som
färdiga binärer:

```powershell
. .\core\dev.ps1
cmake --preset windows -S core
cmake --build core/build/windows
ctest --test-dir core/build/windows --output-on-failure
```

Utdata hamnar i `core/build/windows/bin`: `FubarDocumentCompare.Native.dll`, `printcmp-cli.exe` och beroendena.
.NET-projekten kopierar dem därifrån (`FdcNativeDir`).

```bash
dotnet test bindings/dotnet/FubarDocumentCompare.slnx -c Release
```

Kommandoraden tar en JobSpec (`printcmp.jobspec/1`) eller två PDF:er:

```bash
core/build/windows/bin/printcmp-cli run jobspec.json --out ut
```

```bash
core/build/windows/bin/printcmp-cli doc manus.pdf bok.pdf --layout reflow --out ut
```

```bash
core/build/windows/bin/printcmp-cli data etikett.pdf förväntat.txt
```

## Typsnitt som inte är inbäddade

PDF:er med typsnitt som inte är inbäddade renderas med en ersättning, och då stämmer bilden inte
med trycket. Anroparen kan tillhandahålla fontfilerna som en **uppsättning** (`FontSet`).

- **Uppsättningen är data.**
  - Den är oföränderlig och identifieras av sitt innehåll (hash över filer, alias och reservval).
  - Den anges per anrop: `RenderOptions.Fonts`, `OverlayOptions.Fonts`,
    `GetMetadataAsync(pdf, fonts)`, `ReferenceOptions.Fonts`.
  - Biblioteket vet inget om kunder. Appen laddar kundens typsnitt och skickar med dem.
- **Matchning:** PDF:ens namn (utan delmängdsprefix) mot filens PostScript-namn, alias och
  fullständiga namn. Därefter görs samma jämförelse utan skiljetecken och versaler.
- **Så hittar PDFium filen:**
  - I en kopia av PDF:en byts namnet på typsnitt som inte är inbäddade till en token
    (`FDCF<hash>_<namn>`).
  - Bibliotekets fontuppslag i PDFium (`FPDF_SYSFONTINFO`) lämnar ut filen för tokenet.
  - Därför används samma typsnittsnamn med olika filer, t.ex. två kunders "Acme Sans",
    parallellt utan att blandas ihop.
  - Uppslaget rapporterar också ett namn som är unikt per fil, eftersom PDFium cachar laddade
    typsnitt efter namn.
  - Textlager, överlägg och metadata visar PDF:ens ursprungliga namn.
- **Reservval** för typsnitt som saknas i uppsättningen:
  - `Builtin` (standard) ger PDFiums inbyggda ersättningar, med samma resultat på alla maskiner.
  - `System` ger operativsystemets typsnitt.
  - De 14 standardtypsnitten (och alias som Arial) behåller PDFiums inbyggda mått.
- **Status per typsnitt i metadata:**
  - `embedded`, `supplied` (med filens SHA-256), `standard`, `fallback` eller `system`.
  - `font.not_embedded` varnar bara när typsnittet ersätts.
- **Kontroll innan en fil sparas:** `FontValidator.ValidateAsync` jämför stegbredderna PDF:en
  räknar med mot filens (±2 % av em) och letar efter tecken som saknas. På så sätt upptäcks fel
  typsnitt eller fel version innan filen sparas.

```csharp
// appens kod: kundens typsnitt (t.ex. ur S3), cachade per version
var fonts = engine.FontSets.Create(await LoadTenantFontSources(tenant), FontFallback.Builtin);

var check = await engine.FontValidator.ValidateAsync(pdf, "AcmeSans-Regular", upload);
if (!check.Ok) Console.WriteLine($"fel fil: {check.WidthMismatches} tecken med fel bredd");

var meta = await engine.PdfInspector.GetMetadataAsync(pdf, fonts);   // status per typsnitt
using var img = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 150, Fonts = fonts });
using var withExtra = engine.FontSets.Combine(jobFonts, fonts);     // jobbets typsnitt före kundens
```

Jobbfil (`printcmp-cli run`):

```json
"fonts": { "dir": "fonts", "files": [{ "path": "AcmeSans.otf", "aliases": ["AcmeSans-Regular"] }], "fallback": "builtin" }
```

C: `fdc_fontset_create`/`_add`/`_combine` (varje ändring ger en ny uppsättning),
`fdc_render_options.fonts`, `fdc_reference_options.fonts`, `fdc_pdf_describe(e, pdf, fonts, &json)`
och `fdc_font_validate`.

Begränsningar:

- TTC-samlingar stöds inte; dela upp dem i TTF/OTF.
- Breddkontrollen mäter avståndet mellan teckenorigo. Teckenavstånd (`Tc`) räknas därför in.
- PDFium behåller laddade typsnitt i sin cache så länge processen lever.

## Bildbehandling på områden (`pc_imgproc`)

Filter, FFT och mönstersökning på hela bilder eller på valda områden. Resultatet kan skrivas
tillbaka, flyttas eller skalas till en annan bild, eller hamna i andra områden.

| Modul | Innehåll |
|---|---|
| `region.hpp` | `Region`: rektangel, polygon, cirkel, roterad ruta, mask eller mm-ruta. `RegionSet`: områden ur en mask, rutnät, union |
| `filter.hpp` | `Filter`-gränssnittet (`apply`, `margin`, `name`) och filtren nedan |
| `apply.hpp` | `apply_regions`, `filter_regions`, `apply_to_targets`, `paste`, `Placement` (förskjutning och skala) |
| `fft.hpp` | spektrum, frekvensfilter, notch och `descreen`, rastertoppar och period, DFT-faltning, faskorrelation |
| `match.hpp` | `find_template`, `find_features` (ORB och homografi, flera förekomster), `match_map`, `peaks` |
| `measure.hpp` | `measure` (yta, medel, std, min/max, tyngdpunkt), `blobs`, `draw_regions`, `draw_matches`, `overlay`, `visualize` |
| `transform.hpp` | `rotate` (även utan beskärning), `rotate90`, `flip`, `scale`, `resize_to`, `crop`, `warp_affine`, `warp_quad` (fyrhörning till upprätt bild), `estimate_skew` och `deskew` |

Filtren:

- **Matriser:** egen matris eller separerbar.
- **Utjämning:** gauss, box, median, bilateral.
- **Kanter:** sobel, scharr, laplace, gradient, canny.
- **Ton och skärpa:** oskarp mask, invertering, gråskala, gamma, min–max, CLAHE.
- **Morfologi:** erosion, dilatation, öppning, stängning, gradient, top-hat.
- **Tröskling:** fast, Otsu, adaptiv.
- **Frekvens:** låg-, hög-, band- och bandspärrfilter.
- **Sammansatta och egna:** `chain()` och `custom()`.

Nya filter ärver `Filter`; inget annat behöver ändras.

Varje område filtreras med filtrets marginal av omgivande pixlar. Resultatet inuti området blir
därför detsamma som när hela bilden filtreras, utan skarvar vid områdets kant:

- **Flyttal:** exakt.
- **8 bitar:** högst 1 nivås skillnad, eftersom OpenCV kan välja en annan heltalsväg.
- **Undantag:** filter som räknar på områdets egen statistik (Otsu, min–max, CLAHE) gäller per område.

```cpp
using namespace pc::imgproc;
RegionSet areas = {Region::circle({300, 300}, 200), Region::mm(20, 15, 30, 10, 600)};
cv::Mat edges = apply_regions(*chain({gaussian(1.5), gradient_magnitude()}), img, areas);
apply_regions(*unsharp(1.0, 2.0), img, areas, other_img, Placement{{100, 0}, 0.5});

MatchOptions o;
o.angles = {-4, -2, 0, 2, 4};
o.search_in = areas;
for (auto& m : find_template(sheet, logo, o)) ...   // ruta, hörn, vinkel, skala, poäng

double period = pc::fft::dominant_period(patch);    // rasterperiod i px
cv::Mat clean = pc::fft::descreen(patch);           // tryckrastret bort, per färgkanal
```

Mallsökningen går grovt till fint: först i en nedskalad bild, sedan förfinas varje kandidat i
full upplösning. Ett ark i 600 dpi med åtta etiketter och fem vinklar tar ~4 s (30 s utan).

`printcmp-img` provar samma sak på filer och skriver resultatet som JSON:

```bash
core/build/windows/bin/printcmp-img filter in.png ut.png --filter "gaussian:1.5+gradient" --region circle:300,300,200 --region box:400,400,900,500
```

```bash
core/build/windows/bin/printcmp-img find ark.tif mall.png ut.png --angles -2:2:1 --threshold 0.7
```

```bash
core/build/windows/bin/printcmp-img fft utsnitt.png spektrum.png
```

```bash
core/build/windows/bin/printcmp-img descreen utsnitt.png utan_raster.png
```

`descreen` tar bort det fina rastret, 3–7 px vid 600 dpi. Grövre moiré mellan färgplåtarna är
lågfrekvent innehåll i bilden och går inte att filtrera bort utan att även text försvinner.

### Snedhet, transformer och kedjor i C-API:t och .NET

**Snedheten** hittas automatiskt ur kanternas riktningar (textrader, linjer, etikett- och arkkanter).

- Först ett grovt histogram över gradientriktningarna, modulo 90°.
- Sedan förfining med projektionsprofiler: kantkartan vrids i steg om 0,05° kring den grova
  vinkeln, och den vinkel där rad- och kolumnsummorna är skarpast vinner. Gradienten ensam dras mot
  axlarna av trappstegen i interpolerade kanter.
- Testerna träffar inom ±0,15° (0–11,5°).

**Etiketterna** kan hittas utan jämförelse: `compare::locate_labels`, `fdc_reference_locate` och
`Comparer.LocateLabels`.

- Resultatet är läge, hörn, rad och kolumn, och vridning 0–360° (även 90/180/270°).
- Vinkeln förfinas per etikett: etiketten rätas upp med de grova hörnen och snedheten mäts igen.
- Med hörnen ger `WarpQuad` etiketten upprätt i originalets storlek.

**Kedjor** körs i ett anrop: `fdc_image_process` tar operationerna som JSON, och .NET bygger dem med
`ImageChain`:

```csharp
var chain = ImageChain.Build(c => c
    .Deskew()                                                   // automatisk snedhet
    .On([ImageRegion.Rect(0, 0, 800, 400)], r => r.Gaussian(2))  // bara i området
    .Kernel(new double[,] { { 0, -1, 0 }, { -1, 5, -1 }, { 0, -1, 0 } })
    .Gray().AdaptiveThreshold(31, 10));
using var result = engine.ImageProcessor.Apply(scan, chain, out var notes);   // notes.Deskewed: vinkeln
var skew = engine.ImageProcessor.EstimateSkew(scan);

var sheet = engine.Comparer.LocateLabels(reference, scan);                     // vridning per etikett
foreach (var l in sheet.Labels)
    using (var upright = engine.ImageProcessor.Apply(scan, ImageChain.Build(c => c.WarpQuad(l.Corners)))) { … }
```

| Slag | Operationer |
|---|---|
| Ton | `Gray`, `Invert`, `Gamma`, `Normalize`, `Clahe`, `Linear` (kontrast/ljushet) |
| Utjämning, skärpa | `Gaussian`, `Box`, `Median`, `Bilateral`, `Sharpen` |
| Kanter, matriser | `Sobel`, `Laplacian`, `Gradient`, `Edges` (Canny), `Kernel` (egen matris) |
| Tröskel, morfologi | `Threshold`, `Otsu`, `AdaptiveThreshold`, `Morph` (erode, dilate, open, close, gradient, top-hat, black-hat) |
| Frekvensplanet | `LowPass`, `HighPass`, `BandPass`, `BandStop`, `Descreen` (tar bort tryckrastret) |
| Geometri | `Rotate`, `Rotate90`, `Flip`, `Scale`, `Resize`, `Crop`, `Deskew`, `WarpQuad`, `WarpAffine` |

- Filter kan begränsas till områden (`On(regions, …)`: rektangel, polygon, cirkel); resten av
  bilden lämnas orörd.
- Geometri gäller alltid hela bilden.
- `ImageCodec.Load` läser TIFF/PNG/JPEG och `ImageProcessor.FromPixels` tar egna pixlar.

## Stanslinjer, lack och ISO 19593-bearbetningssteg

Det som inte trycks känns igen på två sätt:

- **Dekorfärgens namn**, enligt standardreglerna (första träffen gäller):
  - punktskrift: `braille|punktskrift|blindskrift|emboss|prägl`;
  - stanslinje: `cut|die|stans|kiss|crease|perf|structural`;
  - lack: `varnish|lack|foil|folie|hot stamp`, men inte "Black" (en svart dekorfärg).
- **ISO 19593-1-bearbetningssteg:** lager (OCG) med `/GTS_Metadata` (`GTS_ProcStepsGroup` och
  `GTS_ProcStepsType`).
  - Allt innehåll i ett sådant lager får stegets roll, oavsett färg:
    - `Structural` ger stanslinje (`dieline`);
    - `Braille` ger punktskrift;
    - övriga (mått, positioner, förklaringar) ger `technical`.
  - Innehållet tas bort ur den sanerade PDF:en som jämförelsen och "bara tryck"-varianten renderar.
  - Metadata visar steget per lager (`processingStep: {group, type}`).

### Verkliga tryckfiler (valfritt test)

`realworld` kör mot verkliga, offentliga tryckfiler:

- Ghent Workgroups Processing Steps Test Suite (ISO 19593-lager för förpackningar);
- Ghent Workgroups PDF Output Suite 5.0, testsidorna (PDF/X-4);
- Averys mallar för etiketter på rulle (DIELINE, BLEEDLINE och SAFELINE som dekorfärger).

Filerna saknar villkor för vidarespridning. De hämtas därför till byggkatalogen och kontrolleras mot
SHA-256 (`core/tests/realworld/fetch.cmake`), men checkas aldrig in:

```powershell
cmake --preset windows -S core -D FDC_REALWORLD_TESTS=ON
cmake --build core/build/windows
ctest --test-dir core/build/windows -L realworld --output-on-failure
```

Testet kontrollerar att varje fil går att läsa, rendera och kartlägga. Det kontrollerar också att
alla 40 patchar med ett Structural-lager ger stanslinjer, att dekorfärgen "Black" inte blir lack och
att Averys DIELINE blir stanslinje.

## Testmaterial och utvärdering

Testmaterialet genereras med facit (`truth.json`), analyseras av kärnan och mäts mot facit.
Allt är C++; samma frö ger byte för byte samma filer.

```bash
core/build/windows/bin/printcmp-testgen labels --out cases --n 6 --seed 300 --scanner ccd --difficulty normal
```

```bash
core/build/windows/bin/printcmp-eval scan cases --run --verbose --json metrics.json --jobs 4
```

```bash
core/build/windows/bin/printcmp-testgen docs --out doccases --books 15 --labels 24 --clean 6 --seed 7000
```

```bash
core/build/windows/bin/printcmp-eval doc doccases --run --verbose --json docmetrics.json
```

```bash
core/build/windows/bin/printcmp-eval bench --out bench --jobs 4
```

- **`printcmp-testgen labels`:** ark med 8 etiketter.
  - Etiketterna ritas med egen PDF-skrivare och felkatalogen ändrar originalet (systematiska fel).
  - Trycket simuleras: raster, punktförstoring, passningsfel och lokala tryckfel.
  - Scannern simuleras: v1, eller en CIS- eller CCD-flatbädd med fysiska veck, damm och brus.
- **`printcmp-testgen docs`:** manus mot satt bok eller tidning, etikettversioner och filer med
  förväntade uppgifter.
- **`printcmp-eval bench`:** de sju fasta uppsättningarna i `docs/algoritmer.md`. Den genererar,
  analyserar och utvärderar i samma process. Resultatet hamnar i `<out>/<uppsättning>/metrics.json`
  och `<out>/summary.json`.

Mallarna och felkatalogen beskrivs i `docs/algoritmer.md`.

**Fast benchmark** (`printcmp-eval bench`, 32 ark och 288 etiketter genererade av C++-generatorn,
600 dpi, 8 etiketter per ark):

| Uppsättning | Etiketter | Originalfel | Tryckfel | Falsklarm/etikett |
|---|---|---|---|---|
| cis_normal | 48 | 64/64 | 10/11 | 0,00 |
| cis_hard | 32 | 32/32 | 3/3 | 0,06 |
| ccd_normal | 48 | 64/64 | 10/11 | 0,00 |
| ccd_hard | 32 | 32/32 | 3/3 | 0,00 |
| cis_fold05 | 32 | 64/64 | 5/6 | 0,03 |
| ccd_fold10 | 32 | 64/64 | 5/6 | 0,50 |
| ccd_fold15 | 32 | 40/48 | 10/12 | 1,28 |

**PDF mot PDF** (`printcmp-testgen docs --seed 7000`, 45 fall):

| | Ändringar | Falsklarm | Data |
|---|---|---|---|
| Böcker och tidningar (21) | 135/135 | 3 | – |
| Etikettversioner (24) | 64/64 | 2 | 150/150, 24/24 rätt variant |

Svagheterna som återstår:

- **Kraftiga CCD-veck (lyft 1–1,5 mm):** ett veck förskjuter allt innehåll på ena sidan, men
  passningen anpassar en modell för hela etiketten. Det ger falsklarm, och vid 1,5 mm missas
  fel nära vecket.
  - Talen är sämre än på prototypens ark, men det är andra ark: samma frön ger nu andra veck.
  - Nästa steg är passning per veckhalva.
- **Dokumentläget:**
  - Ett flyttat element som hamnar intill en tabellrad får raden att rapporteras som borttagen
    och tillagd.
  - På en tidningssida rapporteras tre oförändrade stycken som flyttade.
  - Prototypen gör likadant på samma material, så det är svagheter i analysen och inte
    porteringsfel.

### Paritet med Python-prototypen (historik)

Algoritmerna utvecklades först i Python och portades sedan till C++. Prototypen finns inte
längre i repot; allt, även testmaterial och utvärdering, är C++. Under porteringen kördes båda
mot samma material:

- **Dokumentläge och datakontroll:** samma fynd i alla 159 fall.
- **Scannade ark:** samma fynd. Falsklarmen skilde bara på ark med kraftiga CCD-veck
  (prototypens veckkandidater kom ur en slumpad Hough-transform).
- **Utvärderingen:** C++ och Python gav identiska mätetal på samma resultat (7 + 4 uppsättningar).

Testgeneratorn drar slumptal på ett annat sätt än numpy. Samma frö ger därför andra, men
likvärdiga, testfall än prototypen gav.

**Tid:** ett ark med 8 etiketter i 600 dpi tar 41 s i kärnan mot 89 s i prototypen (en körning
åt gången). Passningen (lokalt flöde) står för 21 s. Att generera ett sådant ark tar ~10 s.

## Kända begränsningar

- Bara Windows/MSVC är byggt och provat. Koden är portabel C++20, men förinställningar för
  Linux/macOS saknas.
- Registreringen är inte optimerad; den står för halva tiden per ark.
- Utsnitt (`fdc_result_render_crop`) finns för etikett/ark, inte för dokumentläget.
- Övriga begränsningar i analysen står i `docs/algoritmer.md`.
