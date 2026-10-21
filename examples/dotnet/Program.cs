// Körbara exempel på vanliga uppgifter med FubarDocumentCompare. Varje fil i den här katalogen är ett
// fristående exempel; Program väljer exempel efter namn.
//
//   dotnet run --project examples/dotnet -- <exempel> [argument …]
//   dotnet run --project examples/dotnet -- all            alla exempel mot testdata/, utdata i en temporär katalog
using Examples;
using FubarDocumentCompare;

var examples = new Dictionary<string, (string Usage, Func<DocumentCompareEngine, string[], Task> Run)>
{
    ["metadata"] = ("<pdf>", (e, a) => Metadata.Run(e, a[0])),
    ["render"] = ("<pdf> <ut.png> [dpi]", (e, a) => Render.Run(e, a[0], a[1], a.Length > 2 ? double.Parse(a[2]) : 150)),
    ["variants"] = ("<pdf> <utkatalog>", (e, a) => Variants.Run(e, a[0], a[1])),
    ["tiles"] = ("<pdf> <utkatalog> [nivå]", (e, a) => Tiles.Run(e, a[0], a[1], a.Length > 2 ? int.Parse(a[2]) : 3)),
    ["overlays"] = ("<pdf>", (e, a) => Overlays.Run(e, a[0])),
    ["fonts"] = ("<pdf> <typsnittsnamn> <fontfil> <ut.png>", (e, a) => Fonts.Run(e, a[0], a[1], a[2], a[3])),
    ["tenant-fonts"] = ("<fontkatalog per kund> <kund> <pdf> <ut.png>", (e, a) => TenantFontsExample.Run(e, a[0], a[1], a[2], a[3])),
    ["storage"] = ("<lagerkatalog> <pdf>", (e, a) => Storage.Run(e, a[0], a[1])),
    ["compare"] = ("<original.pdf> <prov (pdf/tif/png)> <utkatalog>", (e, a) => Compare.Run(e, a[0], a[1], a[2])),
    ["documents"] = ("<manus.pdf> <bok.pdf>", (e, a) => Documents.Run(e, a[0], a[1])),
    ["data-check"] = ("<pdf> <förväntat.txt|csv|json>", (e, a) => DataCheck.Run(e, a[0], a[1])),
    ["gallery"] = ("<pdf> <utkatalog> [dpi]", (e, a) => Gallery.Run(e, a[0], a[1], a.Length > 2 ? double.Parse(a[2]) : 200)),
    ["queue"] = ("<original.pdf> <prov> [<prov> …]", (e, a) => Queue.Run(e, a[0], a[1..])),
};

if (args.Length == 0 || (args[0] != "all" && !examples.ContainsKey(args[0])))
{
    Console.WriteLine("exempel:");
    foreach (var (name, (usage, _)) in examples) Console.WriteLine($"  {name,-13} {usage}");
    Console.WriteLine("  all           alla exempel mot testdata/");
    return 1;
}

using var engine = new DocumentCompareEngine(new EngineOptions { MaxParallelJobs = 2 });

if (args[0] == "all")
{
    var data = TestData.Root();
    var label = Path.Combine(data, "label");
    var acme = Path.Combine(data, "fonts", "acme-sans.pdf");
    var outDir = Directory.CreateTempSubdirectory("fdc-examples-").FullName;
    var fontDir = Path.Combine(outDir, "kundfonter", "acme");
    Directory.CreateDirectory(fontDir);
    File.Copy(TestData.Font("DejaVuSans.ttf"), Path.Combine(fontDir, "AcmeSans-Regular.ttf"));
    var runs = new (string Name, string[] Args)[]
    {
        ("metadata", [Path.Combine(label, "ref.pdf")]),
        ("render", [Path.Combine(label, "ref.pdf"), Path.Combine(outDir, "ref.png")]),
        ("variants", [Path.Combine(label, "ref.pdf"), Path.Combine(outDir, "varianter")]),
        ("tiles", [Path.Combine(label, "ref.pdf"), Path.Combine(outDir, "rutor"), "2"]),
        ("overlays", [Path.Combine(label, "ref.pdf")]),
        ("fonts", [acme, "AcmeSans-Regular", TestData.Font("DejaVuSans.ttf"), Path.Combine(outDir, "acme.png")]),
        ("tenant-fonts", [Path.Combine(outDir, "kundfonter"), "acme", acme, Path.Combine(outDir, "acme-kund.png")]),
        ("storage", [Path.Combine(outDir, "lager"), Path.Combine(label, "ref.pdf")]),
        ("compare", [Path.Combine(label, "ref.pdf"), Path.Combine(label, "smp.pdf"), Path.Combine(outDir, "jämförelse")]),
        ("documents", [Path.Combine(data, "book", "manuscript.pdf"), Path.Combine(data, "book", "book.pdf")]),
        ("data-check", [Path.Combine(label, "smp.pdf"), Path.Combine(label, "expected.txt")]),
        ("gallery", [Path.Combine(label, "smp.pdf"), Path.Combine(outDir, "galleri")]),
        ("queue", [Path.Combine(label, "ref.pdf"), Path.Combine(label, "smp.pdf"), Path.Combine(label, "ref.pdf"), Path.Combine(label, "smp.pdf")]),
    };
    foreach (var (name, a) in runs)
    {
        Console.WriteLine($"\n=== {name} ===");
        await examples[name].Run(engine, a);
    }
    Console.WriteLine($"\nutdata: {outDir}");
    return 0;
}

await examples[args[0]].Run(engine, args[1..]);
return 0;

namespace Examples
{
    internal static class TestData
    {
        public static string Root()
        {
            for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
                if (Directory.Exists(Path.Combine(d.FullName, "testdata"))) return Path.Combine(d.FullName, "testdata");
            throw new DirectoryNotFoundException("testdata");
        }

        public static string Font(string file)
        {
            foreach (var d in new[]
                     {
                         Environment.GetEnvironmentVariable("PRINTCMP_FONT_DIR"),
                         Environment.GetFolderPath(Environment.SpecialFolder.Fonts),
                         Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Microsoft", "Windows", "Fonts"),
                         "/usr/share/fonts/truetype/dejavu",
                     })
                if (d is not null && File.Exists(Path.Combine(d, file))) return Path.Combine(d, file);
            throw new FileNotFoundException(file);
        }
    }
}
