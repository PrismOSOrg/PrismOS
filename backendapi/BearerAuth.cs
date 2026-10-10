namespace PrismPackageRepo;

internal static class BearerAuth
{
    public static IResult Challenge(HttpContext context)
    {
        context.Response.Headers.WWWAuthenticate = "Bearer";
        return Results.Unauthorized();
    }

    public static async Task<(bool Provided, string? Username)> ResolveAsync(HttpContext context,
        AccountStore accounts)
    {
        string authorization = context.Request.Headers.Authorization.ToString();
        if (string.IsNullOrWhiteSpace(authorization)) return (false, null);
        const string prefix = "Bearer ";
        if (!authorization.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            return (true, null);
        string token = authorization[prefix.Length..].Trim();
        if (token.Length == 0) return (true, null);
        string? username = await accounts.ResolveSessionAsync(token, context.RequestAborted);
        return (true, username);
    }
}
