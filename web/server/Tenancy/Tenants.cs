using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;

namespace FubarDocumentCompare.Web.Tenancy;

/// <summary>Var kund-id:t kommer ifrån. Biblioteket vet inget om kunder; det gör bara webblagret.</summary>
public sealed class TenancyOptions
{
    /// <summary>Anspråket (claim) i en inloggad användares token som anger kunden.</summary>
    public string Claim { get; set; } = "tenant";
    /// <summary>Tillåt kund-id i en rubrik (för utveckling och tester, eller bakom en betrodd gateway).</summary>
    public bool AllowHeader { get; set; }
    public string Header { get; set; } = "X-Tenant-Id";
    /// <summary>Kund när inget annat anges (en installation med en kund). null = kräv kund.</summary>
    public string? Default { get; set; }
}

public interface ITenantResolver
{
    /// <summary>Kund-id:t för anropet, eller null om det inte går att avgöra.</summary>
    string? Resolve(HttpContext context);
}

public sealed partial class TenantResolver(IOptions<TenancyOptions> options) : ITenantResolver
{
    public string? Resolve(HttpContext context)
    {
        var o = options.Value;
        string? id = null;
        if (context.User.Identity?.IsAuthenticated == true) id = context.User.FindFirst(o.Claim)?.Value;
        if (id is null && o.AllowHeader && context.Request.Headers.TryGetValue(o.Header, out var h)) id = h.ToString();
        id ??= o.Default;
        return id is not null && Valid().IsMatch(id) ? id : null;
    }

    [GeneratedRegex("^[a-z0-9][a-z0-9_-]{0,63}$")]
    private static partial Regex Valid();
}

public static class TenantExtensions
{
    private const string Key = "fdc.tenant";

    /// <summary>Kräver en kund för alla slutpunkter i gruppen (401 annars) och gör den tillgänglig via <see cref="Tenant"/>.</summary>
    public static RouteGroupBuilder RequireTenant(this RouteGroupBuilder group) =>
        group.AddEndpointFilter(async (ctx, next) =>
        {
            var http = ctx.HttpContext;
            var tenant = http.RequestServices.GetRequiredService<ITenantResolver>().Resolve(http);
            if (tenant is null)
                return Results.Problem(statusCode: 401, title: "kund saknas",
                                       detail: "anropet anger ingen giltig kund (inloggning eller rubrik)");
            http.Items[Key] = tenant;
            return await next(ctx);
        });

    public static string Tenant(this HttpContext context) =>
        context.Items[Key] as string ?? throw new InvalidOperationException("slutpunkten saknar RequireTenant()");
}
