# Exempel: FubarDocumentCompare i C#

Körbara exempel på vanliga uppgifter. Varje exempel är en kort, fristående klass. Alla kan köras
mot `testdata/`:

```bash
dotnet run --project examples/dotnet -- all
```

Ett exempel i taget:

```bash
dotnet run --project examples/dotnet -- render testdata/label/ref.pdf ref.png 300
```

| Uppgift | Exempel | Fil |
|---|---|---|
| Läsa sidor, rutor, lager, dekorfärger, typsnitt och varningar | `metadata` | `PdfExamples.cs` |
| Rendera en sida till PNG | `render` | `PdfExamples.cs` |
| En bild per lager eller dekorfärgsvariant, genomskinlig bakgrund | `variants` | `PdfExamples.cs` |
| Rutpyramid för en zoombar visare, välja nivå efter skärmen | `tiles` | `PdfExamples.cs` |
| Bildkarta (ord, former, streckkoder) i mm och px | `overlays` | `PdfExamples.cs` |
| Hitta typsnitt som saknas, kontrollera en fontfil, rendera med den | `fonts` | `FontExamples.cs` |
| Typsnitt per kund, cachade per version (appens kod) | `tenant-fonts` | `FontExamples.cs` |
| PDF:er ur ett objektlager (S3): hämtas bara när de inte är cachade | `storage` | `CompareExamples.cs` |
| Tryck mot original med förlopp, fynd och export (JSON, HTML, utsnitt) | `compare` | `CompareExamples.cs` |
| Manus mot satt bok | `documents` | `CompareExamples.cs` |
| Förväntade uppgifter i en PDF | `data-check` | `CompareExamples.cs` |
| Jämförelser i bakgrunden: kö, arbetare, förlopp, avbrott | `queue` | `CompareExamples.cs` |
| Galleri: rutor, bara tryck, stanslinje, lackmask, utstansad (genomskinlig utanför stanslinjen), beskuren, annoterad, ett lager i taget | `gallery` | `GalleryExample.cs` |

## Mönster

- **En motor per process.** `DocumentCompareEngine` är trådsäker. Tjänsterna hämtas från den:
  `PdfInspector`, `PdfRenderer`, `ImageCodec`, `FontSets`, `FontValidator`, `Comparer` och
  `ResultExporter`.
- **Data skilt från tjänster.**
  - `PdfSource`, `FontSet`, `Image`, `Reference` och `Result` är data.
  - Operationerna finns i tjänsterna, t.ex. `PdfRenderer.RenderAsync(pdf, options)` och
    `ResultExporter.CropPng(result, …)`.
- **Tillståndslöst.** En PDF anges med innehållets SHA-256 och ett sätt att hämta det.
  - Motorns cache gör att upprepade anrop inte hämtar eller tolkar PDF:en igen.
  - Innehållet kontrolleras mot SHA-256.
- **Kunder finns bara i appen.** Appen laddar kundens typsnitt (`LoadTenantFonts`) och skickar med
  dem i `RenderOptions.Fonts`, `OverlayOptions.Fonts`, `GetMetadataAsync(pdf, fonts)` och
  `ReferenceOptions.Fonts`.
- **Frigör det som håller inbyggt minne:** `Image`, `FontSet`, `Reference` och `Result` (`using`).
  Pågående och cachade anrop har egna referenser, så en `FontSet` kan frigöras när som helst.
