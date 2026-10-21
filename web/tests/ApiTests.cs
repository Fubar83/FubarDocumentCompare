using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Xunit;

namespace FubarDocumentCompare.Web.Tests;

/// <summary>En server mot ett eget lager i en temporär katalog (två servrar kan dela samma lager).</summary>
public sealed class Server : WebApplicationFactory<Program>
{
    public Server(string? root = null) => Root = root ?? Directory.CreateTempSubdirectory("fdc-web-").FullName;

    public string Root { get; }

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        builder.UseEnvironment("Testing");
        builder.UseSetting("Storage:Kind", "FileSystem");
        builder.UseSetting("Storage:Root", Path.Combine(Root, "blobs"));
        builder.UseSetting("Legacy:WorkDir", Path.Combine(Root, "legacy"));
        builder.UseSetting("Tenancy:AllowHeader", "true");
        builder.UseSetting("Fonts:ManifestTtl", "00:00:00");
    }

    public HttpClient For(string tenant)
    {
        var c = CreateClient();
        c.DefaultRequestHeaders.Add("X-Tenant-Id", tenant);
        return c;
    }

    public static string Data(params string[] parts)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var p = Path.Combine(d.FullName, "testdata");
            if (Directory.Exists(p)) return Path.Combine([p, .. parts]);
        }
        throw new DirectoryNotFoundException("testdata");
    }

    public static string Font(string file)
    {
        foreach (var d in new[]
                 {
                     Environment.GetEnvironmentVariable("PRINTCMP_FONT_DIR"),
                     Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Microsoft", "Windows", "Fonts"),
                     Environment.GetFolderPath(Environment.SpecialFolder.Fonts),
                     "/usr/share/fonts/truetype/dejavu",
                 })
            if (d is not null && File.Exists(Path.Combine(d, file))) return Path.Combine(d, file);
        throw new FileNotFoundException(file);
    }
}

public class ApiTests : IClassFixture<ApiTests.Shared>
{
    public sealed class Shared : IDisposable
    {
        public Server Server { get; } = new();
        public void Dispose() => Server.Dispose();
    }

    private readonly Server _server;

    public ApiTests(Shared s) => _server = s.Server;

    private static MultipartFormDataContent Upload(string path, params (string Name, string Value)[] fields)
    {
        var form = new MultipartFormDataContent();
        var file = new ByteArrayContent(File.ReadAllBytes(path));
        file.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        form.Add(file, "file", Path.GetFileName(path));
        foreach (var (n, v) in fields) form.Add(new StringContent(v), n);
        return form;
    }

    private static async Task<JsonElement> Json(HttpResponseMessage r) => JsonDocument.Parse(await r.Content.ReadAsStringAsync()).RootElement;

    private async Task<string> UploadDocument(HttpClient c, string path)
    {
        var r = await c.PostAsync("/api/documents", Upload(path));
        Assert.True(r.IsSuccessStatusCode, await r.Content.ReadAsStringAsync());
        return (await Json(r)).GetProperty("document").GetProperty("id").GetString()!;
    }

    [Fact]
    public async Task Upload_is_deduplicated_and_isolated_per_tenant()
    {
        var acme = _server.For("acme");
        var r1 = await acme.PostAsync("/api/documents", Upload(Server.Data("label", "ref.pdf")));
        Assert.Equal(HttpStatusCode.Created, r1.StatusCode);
        var j1 = await Json(r1);
        var id = j1.GetProperty("document").GetProperty("id").GetString()!;
        Assert.StartsWith("doc_", id);
        Assert.Equal(1, j1.GetProperty("metadata").GetProperty("pageCount").GetInt32());
        Assert.True(j1.GetProperty("metadata").GetProperty("pages")[0].GetProperty("widthMm").GetDouble() > 10);

        var r2 = await acme.PostAsync("/api/documents", Upload(Server.Data("label", "ref.pdf")));
        Assert.Equal(HttpStatusCode.OK, r2.StatusCode);
        Assert.Equal(id, (await Json(r2)).GetProperty("document").GetProperty("id").GetString());

        Assert.Equal(HttpStatusCode.NotFound, (await _server.For("other").GetAsync($"/api/documents/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await _server.CreateClient().GetAsync($"/api/documents/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await _server.CreateClient().GetAsync("/api/documents")).StatusCode);
        var list = await Json(await acme.GetAsync("/api/documents"));
        Assert.Contains(list.EnumerateArray(), d => d.GetProperty("id").GetString() == id);
    }

    [Fact]
    public async Task Bad_upload_leaves_nothing_behind()
    {
        var c = _server.For("bad");
        var tmp = Path.GetTempFileName();
        await File.WriteAllTextAsync(tmp, "inte en pdf");
        var r = await c.PostAsync("/api/documents", Upload(tmp));
        Assert.False(r.IsSuccessStatusCode);
        Assert.Equal("application/problem+json", r.Content.Headers.ContentType?.MediaType);
        Assert.Empty((await Json(await c.GetAsync("/api/documents"))).EnumerateArray());
    }

    [Fact]
    public async Task Images_have_etags_and_formats()
    {
        var c = _server.For("img");
        var id = await UploadDocument(c, Server.Data("label", "ref.pdf"));
        var r = await c.GetAsync($"/api/documents/{id}/pages/1/image?dpi=72");
        Assert.Equal(HttpStatusCode.OK, r.StatusCode);
        Assert.Equal("image/png", r.Content.Headers.ContentType?.MediaType);
        var png = await r.Content.ReadAsByteArrayAsync();
        Assert.Equal(0x89, png[0]);
        var etag = r.Headers.ETag!;

        var again = new HttpRequestMessage(HttpMethod.Get, $"/api/documents/{id}/pages/1/image?dpi=72");
        again.Headers.IfNoneMatch.Add(etag);
        Assert.Equal(HttpStatusCode.NotModified, (await c.SendAsync(again)).StatusCode);

        var jpg = await c.GetAsync($"/api/documents/{id}/pages/1/image?dpi=72&format=jpeg&excludeSpots=Varnish&hideNonPrinting=true");
        Assert.Equal("image/jpeg", jpg.Content.Headers.ContentType?.MediaType);
        Assert.NotEqual(etag, jpg.Headers.ETag);

        var part = await c.GetAsync($"/api/documents/{id}/pages/1/image?dpi=72&x=10&y=10&width=50&height=40&pixels=gray");
        Assert.Equal(HttpStatusCode.OK, part.StatusCode);

        var bad = await c.GetAsync($"/api/documents/{id}/pages/1/image?box=nope");
        Assert.Equal(HttpStatusCode.BadRequest, bad.StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await c.GetAsync($"/api/documents/{id}/pages/9/image")).StatusCode);
    }

    [Fact]
    public async Task Tiles_and_overlays()
    {
        var c = _server.For("tiles");
        var id = await UploadDocument(c, Server.Data("label", "ref.pdf"));
        var grid = await Json(await c.GetAsync($"/api/documents/{id}/pages/1/tiles/3?size=128"));
        Assert.Equal(144, grid.GetProperty("dpi").GetDouble());
        int cols = grid.GetProperty("columns").GetInt32();
        Assert.True(cols >= 2);
        var tile = await c.GetAsync($"/api/documents/{id}/pages/1/tiles/3/1/0?size=128");
        Assert.Equal(HttpStatusCode.OK, tile.StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await c.GetAsync($"/api/documents/{id}/pages/1/tiles/3/{cols}/0?size=128")).StatusCode);

        var o = await Json(await c.GetAsync($"/api/documents/{id}/pages/1/overlays"));
        Assert.True(o.GetProperty("words").GetArrayLength() > 0);
        Assert.Equal(0, o.GetProperty("barcodes").GetArrayLength());
        var codes = await Json(await c.GetAsync($"/api/documents/{id}/pages/1/overlays?kinds=barcodes"));
        Assert.True(codes.GetProperty("barcodes").GetArrayLength() > 0);
        Assert.Equal(0, codes.GetProperty("words").GetArrayLength());
    }

    [Fact]
    public async Task Tenant_fonts_are_validated_stored_and_used()
    {
        var c = _server.For("fonts");
        var id = await UploadDocument(c, Server.Data("fonts", "acme-sans.pdf"));
        var meta = await Json(await c.GetAsync($"/api/documents/{id}"));
        var font = meta.GetProperty("metadata").GetProperty("pages")[0].GetProperty("fonts")[0];
        Assert.Equal("AcmeSans-Regular", font.GetProperty("name").GetString());
        Assert.Equal("fallback", font.GetProperty("status").GetString());
        var before = (await c.GetAsync($"/api/documents/{id}/pages/1/image?dpi=100&box=media")).Headers.ETag;

        var check = await Json(await c.PostAsync($"/api/documents/{id}/fonts/validate",
                                                 Upload(Server.Font("DejaVuSans.ttf"), ("fontName", "AcmeSans-Regular"))));
        Assert.True(check.GetProperty("ok").GetBoolean());

        var wrong = await c.PostAsync("/api/fonts", Upload(Server.Font("DejaVuSans-Bold.ttf"), ("documentId", id), ("fontName", "AcmeSans-Regular")));
        Assert.Equal(HttpStatusCode.UnprocessableEntity, wrong.StatusCode);
        var right = await c.PostAsync("/api/fonts", Upload(Server.Font("DejaVuSans.ttf"), ("documentId", id), ("fontName", "AcmeSans-Regular")));
        Assert.Equal(HttpStatusCode.Created, right.StatusCode);
        var sha = (await Json(right)).GetProperty("font").GetProperty("sha256").GetString()!;

        meta = await Json(await c.GetAsync($"/api/documents/{id}"));
        font = meta.GetProperty("metadata").GetProperty("pages")[0].GetProperty("fonts")[0];
        Assert.Equal("supplied", font.GetProperty("status").GetString());
        Assert.Equal(sha, font.GetProperty("suppliedSha256").GetString());
        var after = (await c.GetAsync($"/api/documents/{id}/pages/1/image?dpi=100&box=media")).Headers.ETag;
        Assert.NotEqual(before, after);   // samma adress, annan bild: ETag följer kundens typsnitt

        var list = await Json(await c.GetAsync("/api/fonts"));
        Assert.Equal(1, list.GetProperty("fonts").GetArrayLength());
        Assert.Equal(HttpStatusCode.NoContent, (await c.DeleteAsync($"/api/fonts/{sha}")).StatusCode);
        Assert.Equal(0, (await Json(await c.GetAsync("/api/fonts"))).GetProperty("fonts").GetArrayLength());
        // en annan kund ser aldrig typsnittet
        Assert.Equal(0, (await Json(await _server.For("someone").GetAsync("/api/fonts"))).GetProperty("fonts").GetArrayLength());
    }

    [Fact]
    public async Task Sessions_store_opaque_json_with_optimistic_locking()
    {
        var c = _server.For("ses");
        var body = new StringContent("""{"zoom": 3, "layers": ["Bas"], "notes": [{"x": 1}]}""", Encoding.UTF8, "application/json");
        var created = await c.PostAsync("/api/sessions?documentId=doc_x", body);
        Assert.Equal(HttpStatusCode.Created, created.StatusCode);
        var s = await Json(created);
        var id = s.GetProperty("id").GetString()!;
        var etag = created.Headers.ETag!;
        Assert.Equal(3, s.GetProperty("data").GetProperty("zoom").GetInt32());

        var put = new HttpRequestMessage(HttpMethod.Put, $"/api/sessions/{id}") { Content = new StringContent("""{"zoom": 4}""", Encoding.UTF8, "application/json") };
        put.Headers.IfMatch.Add(etag);
        var updated = await c.SendAsync(put);
        Assert.Equal(HttpStatusCode.OK, updated.StatusCode);
        Assert.NotEqual(etag, updated.Headers.ETag);

        var stale = new HttpRequestMessage(HttpMethod.Put, $"/api/sessions/{id}") { Content = new StringContent("""{"zoom": 5}""", Encoding.UTF8, "application/json") };
        stale.Headers.IfMatch.Add(etag);
        Assert.Equal(HttpStatusCode.PreconditionFailed, (await c.SendAsync(stale)).StatusCode);

        var got = await Json(await c.GetAsync($"/api/sessions/{id}"));
        Assert.Equal(4, got.GetProperty("data").GetProperty("zoom").GetInt32());
        Assert.Equal("doc_x", got.GetProperty("documentId").GetString());
        var list = await Json(await c.GetAsync("/api/sessions?documentId=doc_x"));
        Assert.Single(list.EnumerateArray());
        Assert.Equal(HttpStatusCode.NotFound, (await _server.For("other").GetAsync($"/api/sessions/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest,
                     (await c.PostAsync("/api/sessions", new StringContent("inte json", Encoding.UTF8, "application/json"))).StatusCode);
        Assert.Equal(HttpStatusCode.NoContent, (await c.DeleteAsync($"/api/sessions/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await c.GetAsync($"/api/sessions/{id}")).StatusCode);
    }

    [Fact]
    public async Task Servers_are_stateless_and_share_storage()
    {
        using var other = new Server(_server.Root);   // en andra server mot samma lager
        var a = _server.For("shared");
        var b = other.For("shared");
        var id = await UploadDocument(a, Server.Data("label", "smp.pdf"));
        var img = await b.GetAsync($"/api/documents/{id}/pages/1/image?dpi=50");
        Assert.Equal(HttpStatusCode.OK, img.StatusCode);
        Assert.Equal(HttpStatusCode.NoContent, (await b.DeleteAsync($"/api/documents/{id}")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await a.GetAsync($"/api/documents/{id}")).StatusCode);
        Assert.False(Directory.EnumerateFiles(Path.Combine(_server.Root, "blobs", "tenants", "shared"), "*.pdf", SearchOption.AllDirectories).Any());
    }

    [Fact]
    public async Task Legacy_api_still_works()
    {
        var c = _server.CreateClient();
        var v = await Json(await c.GetAsync("/api/version"));
        Assert.Equal(2, v.GetProperty("api").GetInt32());
        var form = new MultipartFormDataContent { { new ByteArrayContent(File.ReadAllBytes(Server.Data("label", "ref.pdf"))), "pdf", "ref.pdf" } };
        var r = await c.PostAsync("/api/references", form);
        Assert.Equal(HttpStatusCode.OK, r.StatusCode);
    }
}
