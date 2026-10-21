# Testdata

Små fall ur `printcmp-testgen docs --books 1 --labels 1 --seed 7000` (genererade, inga riktiga
produkter) som används av `core/tests` och `bindings/dotnet/FubarDocumentCompare.Tests`. `truth.json` är facit.

- `label/` – etikettmallen oat_drink i två versioner (frö 7100): `ref.pdf` mot `smp.pdf` och
  förväntade uppgifter i `expected.txt`.
- `book/` – manus mot satt bok (frö 7000): `manuscript.pdf` mot `book.pdf`.
- `fonts/acme-sans.pdf` – "Hello World" i typsnittet AcmeSans-Regular, inte inbäddat, med DejaVu
  Sans stegbredder (för tester av typsnittsuppsättningar). Skrivs om med
  `FDC_WRITE_FONT_FIXTURE=1 printcmp_fonts_tests`; testet `fonts_fixture_for_bindings` kontrollerar
  att filen är aktuell.
