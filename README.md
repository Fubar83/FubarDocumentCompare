# printcmp

Jämför tryck och scannade ark mot PDF-original, och PDF mot PDF (manus mot bok, versioner av
en etikett, förväntade uppgifter ur en textfil). Ingen OCR: allt som ska läsas tas ur PDF:ens
textlager eller avkodas (streckkoder).

```
core/              C++20-kärnan: lagrade bibliotek, C-API (FubarDocumentCompare.Native.dll) och printcmp-cli
bindings/dotnet/   .NET-biblioteket FubarDocumentCompare med xUnit-tester; FubarDocumentCompare.slnx bygger allt i .NET
web/server/        webbtjänst FubarDocumentCompare.Web (ASP.NET minimal API, S3/filsystem, Server-Sent Events)
web/tests/         webbtjänstens integrationstester (WebApplicationFactory)
web/client/        webbgränssnitt i TypeScript, byggs till web/server/wwwroot/js
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
  tests/                      ctest: core, capi, smoke, architecture, imgproc, docview, fonts
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
- **Stabil yta utåt:** C-API:t är det enda som exporteras. .NET-bindningen och webbtjänsten
  använder bara det.

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
```

Namnrymder: `FubarDocumentCompare` (motor, fel, inställningar), `.Pdf`, `.Imaging` och
`.Comparison`.

Den inbyggda DLL:en heter `FubarDocumentCompare.Native.dll` och .NET-sammansättningen
`FubarDocumentCompare.dll`.

### Webbtjänsten

`web/server` (FubarDocumentCompare.Web) är tillståndslös. Dokument, kundernas typsnitt och sessioner
ligger i ett objektlager (S3, eller filsystemet vid utveckling), så servrar kan läggas till och tas bort
fritt. Biblioteket vet inget om kunder; webblagret avgör kunden per anrop:

- i första hand ur anspråket `tenant` för en inloggad användare;
- annars ur rubriken `X-Tenant-Id`, om `Tenancy:AllowHeader` är på;
- annars `Tenancy:Default`.

Allt i lagret ligger under `tenants/{kund}/` (se `Storage/Keys.cs`).

| | |
|---|---|
| `POST /api/documents` | PDF (multipart, `file`) → 201 post + metadata; samma innehåll igen → 200 med samma dokument |
| `GET /api/documents`, `GET/DELETE /api/documents/{id}` | lista, post + metadata (typsnittens status mot kundens typsnitt), ta bort |
| `GET …/{id}/pages/{n}/image` | bild: `dpi`, `box`, `pixels` (rgb/rgba/gray), `format` (png/jpeg), `quality`, `x/y/width/height` |
| `GET …/{id}/pages/{n}/tiles/{level}` | rutnätet (`size`, `box`); nivå L = 18 · 2^L dpi |
| `GET …/{id}/pages/{n}/tiles/{level}/{x}/{y}` | en ruta, pixellik med hela sidan |
| `GET …/{id}/pages/{n}/overlays` | bildkarta i mm: `kinds=words,objects,spots,barcodes`, `box` |
| `POST …/{id}/fonts/validate` | fontfil + `fontName` → stämmer filen med typsnittet i PDF:en? |
| `GET/POST /api/fonts`, `DELETE /api/fonts/{sha256}` | kundens typsnitt; med `documentId` + `fontName` kontrolleras filen först (422 om den inte stämmer) |
| `POST/GET /api/sessions`, `GET/PUT/DELETE /api/sessions/{id}` | gränssnittets egna JSON-data per kund; `PUT` med `If-Match` → 412 om någon annan hunnit före |

- **Variant** för bilder, rutor och överlägg: `includeLayers`, `excludeLayers`, `excludeSpots`
  (kommaseparerade) och `hideNonPrinting`.
- **Cachning:** bilder, rutor, överlägg och metadata har en ETag (innehåll + parametrar + kundens
  typsnitt) och `Cache-Control: private, no-cache`.
  - Webbläsaren sparar svaret, och `If-None-Match` ger 304 utan att något renderas.
  - Kundens typsnitt kan ändras utan att adressen ändras, därför frågar webbläsaren varje gång.
- **Fel** returneras som ProblemDetails (`application/problem+json`): 400 ogiltigt, 401 kund saknas,
  404, 412, 415 fel filformat och 422 PDF eller fontfil som inte stämmer.
- **Konfiguration** (appsettings eller miljövariabler, t.ex. `Storage__Kind=S3`):
  - `Storage:Kind` = FileSystem eller S3, `Storage:Root`, `Storage:Bucket`, `Storage:Prefix`,
    `Storage:ServiceUrl` (MinIO, R2 …);
  - `Tenancy:*`;
  - `Fonts:Fallback` (Builtin), `Fonts:ManifestTtl`;
  - `Engine:MaxParallelJobs`, `Engine:CacheBytes`.
  - I Development är rubriken tillåten och kunden heter `dev`.
- **S3 och flera servrar:** villkorade skrivningar (If-None-Match/If-Match) skyddar
  typsnittslistan och sessionerna när flera servrar skriver samtidigt. Filsystemslagret är bara
  atomärt inom en process.

Det första API:et finns kvar (`Legacy/LegacyEndpoints.cs`) tills jämförelserna körs i kö. Det
håller referenser och jobb i minnet på en server, och det nuvarande gränssnittet använder det:

| | |
|---|---|
| `POST /api/references` | PDF (+ profil, box, sida, uteslutna lager/dekorfärger) → lager, dekorfärger, streckkoder, mått |
| `POST /api/jobs/compare` | referens-id + prov (TIFF/PNG/JPEG/PDF) + valfria förväntade uppgifter → jobb-id |
| `POST /api/jobs/documents` | två PDF:er, layout `reflow`/`fixed` → jobb-id |
| `POST /api/jobs/data-check` | PDF + förväntat (.txt/.csv/.json) → jobb-id |
| `GET /api/jobs/{id}/events` | `text/event-stream`, hela förloppet (även vid sen anslutning; `Last-Event-ID` stöds) |
| `GET /api/jobs/{id}/result` | sammanfattning, fynd, etiketter, datafält (JSON) |
| `GET /api/jobs/{id}/crops/{i}/{reference\|sample}` | utsnitt kring ett fynd (PNG) |
| `GET /api/jobs/{id}/report` | HTML-rapporten |
| `DELETE /api/jobs/{id}` | avbryt; ett färdigt jobb städas bort |

Uppladdade filer sparas under ett eget namn per jobb (klientens filnamn används aldrig som
sökväg) och tas bort efter två timmar.

Gränssnittet har tre flikar: etikett/ark, dokument och datakontroll.

- Arket ritas upp medan etiketterna hittas och färgas efter status när de analyserats.
- Fynden kan filtreras på typ och säkerhetsgrad, och varje fynd visar utsnitt ur original
  och prov sida vid sida.
- Godkända fynd (efter fingeravtryck) sparas i webbläsaren och döljs.

## Bygga

Verktyg (winget):

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

```bash
winget install Kitware.CMake Git.Git Microsoft.DotNet.SDK.8 OpenJS.NodeJS.LTS
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

```bash
dotnet run --project web/server -c Release --urls http://localhost:5080
```

Webbservern bygger gränssnittet (`npm install` första gången, sedan `tsc`) när
TypeScript-källorna ändrats; `-p:SkipClientBuild=true` hoppar över det.

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
if (!check.Ok) return Results.BadRequest(check);          // t.ex. 37 tecken med fel bredd

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
- Webbtjänsten har ingen inloggning och håller jobben i minnet. Den är tänkt för ett
  internt nät eller som exempel på hur API:t används.
- Övriga begränsningar i analysen står i `docs/algoritmer.md`.
