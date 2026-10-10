using System.Text;
using System.Threading.RateLimiting;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.RateLimiting;
using PrismPackageRepo;

var builder = WebApplication.CreateBuilder(args);
var repositorySection = builder.Configuration.GetSection("Repository");
string packagesPath = repositorySection["PackagesPath"] ?? "Packages";
int chunkSize = repositorySection.GetValue("ChunkSize", 4096);
int maxCatalogBytes = repositorySection.GetValue("MaxCatalogBytes", 6144);
int maxUploadBytes = repositorySection.GetValue("MaxUploadBytes", 32 * 1024 * 1024);
long maxRepositoryBytes = repositorySection.GetValue("MaxRepositoryBytes", 512L * 1024 * 1024);
string resolvedPackagesPath = Path.IsPathRooted(packagesPath)
    ? packagesPath
    : Path.Combine(builder.Environment.ContentRootPath, packagesPath);
string dataDirectory = builder.Configuration["Security:DataDirectory"] ?? "Data";
string resolvedDataDirectory = Path.IsPathRooted(dataDirectory)
    ? dataDirectory
    : Path.Combine(builder.Environment.ContentRootPath, dataDirectory);
string databasePath = builder.Configuration["Security:DatabasePath"] ?? Path.Combine(dataDirectory, "prismrepo.db");
string resolvedDatabasePath = Path.IsPathRooted(databasePath)
    ? databasePath
    : Path.Combine(builder.Environment.ContentRootPath, databasePath);

if (maxUploadBytes is < 1 or > 256 * 1024 * 1024)
    throw new InvalidDataException("Repository:MaxUploadBytes must be between 1 byte and 256 MiB.");
if (maxRepositoryBytes < maxUploadBytes || maxRepositoryBytes > 16L * 1024 * 1024 * 1024)
    throw new InvalidDataException("Repository:MaxRepositoryBytes must be at least MaxUploadBytes and at most 16 GiB.");
builder.WebHost.ConfigureKestrel(options => options.Limits.MaxRequestBodySize = maxUploadBytes);
builder.Services.AddRateLimiter(options =>
{
    options.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
    options.AddPolicy("auth", context => RateLimitPartition.GetFixedWindowLimiter(
        context.Connection.RemoteIpAddress?.ToString() ?? "unknown",
        _ => new FixedWindowRateLimiterOptions
        {
            PermitLimit = 10,
            Window = TimeSpan.FromMinutes(1),
            QueueLimit = 0,
            AutoReplenishment = true
        }));
});

PackageRepository repository = await PackageRepository.LoadAsync(
    resolvedPackagesPath, chunkSize, maxCatalogBytes);
AccountStore accounts = await AccountStore.LoadAsync(resolvedDatabasePath,
    Path.Combine(resolvedDataDirectory, "accounts.json"));
builder.Services.AddSingleton(repository);
builder.Services.AddSingleton(accounts);

var app = builder.Build();
app.UseRateLimiter();

app.MapGet("/healthz", () => Results.Text("ok\n", "text/plain; charset=utf-8"));

app.MapGet("/api/v1/catalog", (PackageRepository store) =>
    Results.Bytes(store.Catalog.ToArray(), "text/plain; charset=utf-8"));

app.MapGet("/api/v1/packages/{id}/{version}/manifest", (string id, string version,
    PackageRepository store) =>
{
    if (!store.TryGetPackage(id, version, out PackageRecord? package) || package is null)
    {
        return Results.Text("package version not found\n", "text/plain; charset=utf-8", statusCode: 404);
    }
    return Results.Bytes(store.CreateManifest(package), "text/plain; charset=utf-8");
});

app.MapGet("/api/v1/packages/{id}/{version}/chunk", async (string id, string version,
    long? offset, int? length, PackageRepository store, AccountStore accountStore,
    HttpContext context) =>
{
    (bool authProvided, string? username) = await BearerAuth.ResolveAsync(context, accountStore);
    if (authProvided && username is null) return BearerAuth.Challenge(context);
    if (!store.TryGetPackage(id, version, out PackageRecord? package) || package is null)
    {
        return Results.Text("package version not found\n", "text/plain; charset=utf-8", statusCode: 404);
    }
    if (offset is null || length is null || offset < 0 || length < 1 || length > store.ChunkSize)
    {
        return Results.Text("offset and length are required; length must fit the configured chunk size\n",
            "text/plain; charset=utf-8", statusCode: 400);
    }
    if (offset >= package.Size)
    {
        return Results.Text("chunk offset is outside the package\n", "text/plain; charset=utf-8", statusCode: 416);
    }

    byte[]? bytes = await store.ReadChunkAsync(package, offset.Value, length.Value,
        context.RequestAborted);
    if (bytes is null)
        return Results.Text("invalid chunk range\n", "text/plain; charset=utf-8", statusCode: 400);
    try
    {
        await accountStore.RecordDownloadChunkAsync(username, package.Id, package.Version,
            offset.Value, bytes.Length, context.RequestAborted);
    }
    catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
    {
        throw;
    }
    catch (Exception exception)
    {
        app.Logger.LogWarning(exception, "Could not persist download statistics for {PackageId} {Version}",
            package.Id, package.Version);
    }
    return Results.Bytes(bytes, "application/octet-stream");
});

app.MapPost("/api/v1/auth/register", async (CredentialsRequest? request,
    AccountStore store, HttpContext context) =>
{
    if (request is null || request.Username is null || request.Password is null)
        return Results.BadRequest(new { error = "username and password are required" });
    var result = await store.RegisterAsync(request.Username, request.Password, context.RequestAborted);
    if (result.Token is null)
        return Results.BadRequest(new { error = result.Error });
    return Results.Json(new AuthTokenResponse(result.Token, "Bearer", request.Username.Trim().ToLowerInvariant(),
        12 * 60 * 60), statusCode: StatusCodes.Status201Created);
}).WithMetadata(new RequestSizeLimitAttribute(16 * 1024)).RequireRateLimiting("auth");

app.MapPost("/api/v1/auth/login", async (CredentialsRequest? request,
    AccountStore store, HttpContext context) =>
{
    if (request is null || request.Username is null || request.Password is null)
        return Results.BadRequest(new { error = "username and password are required" });
    string? token = await store.LoginAsync(request.Username, request.Password, context.RequestAborted);
    if (token is null) return BearerAuth.Challenge(context);
    return Results.Json(new AuthTokenResponse(token, "Bearer", request.Username.Trim().ToLowerInvariant(),
        12 * 60 * 60));
}).WithMetadata(new RequestSizeLimitAttribute(16 * 1024)).RequireRateLimiting("auth");

app.MapGet("/api/v1/auth/me", async (AccountStore store, HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, store);
    return username is null
        ? BearerAuth.Challenge(context)
        : Results.Json(new { username });
});

app.MapPost("/api/v1/auth/logout", async (AccountStore store, HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, store);
    if (!provided || username is null) return BearerAuth.Challenge(context);
    string authorization = context.Request.Headers.Authorization.ToString();
    await store.LogoutAsync(authorization[7..].Trim(), context.RequestAborted);
    return Results.NoContent();
});

app.MapPost("/api/v1/uploads", async (PackageRepository store, AccountStore accountStore,
    HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, accountStore);
    if (!provided || username is null) return BearerAuth.Challenge(context);
    if (!context.Request.ContentType?.StartsWith("application/octet-stream",
        StringComparison.OrdinalIgnoreCase) ?? true)
        return Results.StatusCode(StatusCodes.Status415UnsupportedMediaType);

    string id = context.Request.Headers["X-Package-Id"].ToString();
    string version = context.Request.Headers["X-Package-Version"].ToString();
    string description = context.Request.Headers["X-Package-Description"].ToString();
    if (string.IsNullOrWhiteSpace(id) || string.IsNullOrWhiteSpace(version))
        return Results.BadRequest(new { error = "X-Package-Id and X-Package-Version are required" });
    if (context.Request.ContentLength > maxUploadBytes)
        return Results.StatusCode(StatusCodes.Status413PayloadTooLarge);

    PublishPackageResult result = await store.PublishAsync(id, version, description,
        username, context.Request.Body, context.Request.ContentLength, maxUploadBytes,
        maxRepositoryBytes, maxCatalogBytes, context.RequestAborted);
    return result.Status switch
    {
        PublishPackageStatus.Success => Results.Json(new
        {
            id = result.Package!.Id,
            version = result.Package.Version,
            size = result.Package.Size,
            sha256 = result.Package.Sha256
        }, statusCode: StatusCodes.Status201Created),
        PublishPackageStatus.InvalidMetadata => Results.BadRequest(new { error = "invalid package ID or version" }),
        PublishPackageStatus.AlreadyExists => Results.Conflict(new { error = "that package version already exists" }),
        PublishPackageStatus.TooLarge => Results.StatusCode(StatusCodes.Status413PayloadTooLarge),
        PublishPackageStatus.RepositoryFull => Results.StatusCode(StatusCodes.Status507InsufficientStorage),
        PublishPackageStatus.Empty => Results.BadRequest(new { error = "empty package uploads are not allowed" }),
        PublishPackageStatus.LengthMismatch => Results.BadRequest(new { error = "request body length did not match Content-Length" }),
        PublishPackageStatus.CatalogFull => Results.StatusCode(StatusCodes.Status507InsufficientStorage),
        _ => Results.StatusCode(StatusCodes.Status500InternalServerError)
    };
});

app.MapGet("/api/v1/users/me/favorites", async (PackageRepository packageStore,
    AccountStore accountStore, HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, accountStore);
    if (!provided || username is null) return BearerAuth.Challenge(context);
    IReadOnlyList<string>? favorites = await accountStore.GetFavoritesAsync(username, context.RequestAborted);
    var packages = favorites!.Select(key =>
    {
        int separator = key.IndexOf('/');
        string id = key[..separator];
        string version = key[(separator + 1)..];
        return packageStore.TryGetPackage(id, version, out PackageRecord? item) ? item : null;
    }).Where(item => item is not null).Select(item => new { item!.Id, item.Version, item.Description });
    return Results.Json(packages);
});

app.MapPut("/api/v1/users/me/favorites/{id}/{version}", async (string id, string version,
    PackageRepository packageStore, AccountStore accountStore, HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, accountStore);
    if (!provided || username is null) return BearerAuth.Challenge(context);
    if (!packageStore.TryGetPackage(id, version, out _)) return Results.NotFound();
    await accountStore.SetFavoriteAsync(username, $"{id.ToLowerInvariant()}/{version}", true,
        context.RequestAborted);
    return Results.NoContent();
});

app.MapDelete("/api/v1/users/me/favorites/{id}/{version}", async (string id, string version,
    AccountStore accountStore, HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, accountStore);
    if (!provided || username is null) return BearerAuth.Challenge(context);
    await accountStore.SetFavoriteAsync(username, $"{id.ToLowerInvariant()}/{version}", false,
        context.RequestAborted);
    return Results.NoContent();
});

app.MapGet("/api/v1/users/me/downloads", async (AccountStore store, HttpContext context) =>
{
    (bool provided, string? username) = await BearerAuth.ResolveAsync(context, store);
    if (!provided || username is null) return BearerAuth.Challenge(context);
    IReadOnlyList<UserDownloadStat>? downloads = await store.GetUserDownloadsAsync(username,
        context.RequestAborted);
    return Results.Json(downloads);
});

app.MapGet("/api/v1/stats/downloads", async (AccountStore store, HttpContext context) =>
    Results.Json(await store.GetDownloadStatsAsync(context.RequestAborted)));

app.Run();

internal sealed record CredentialsRequest(string? Username, string? Password);
internal sealed record AuthTokenResponse(string AccessToken, string TokenType, string Username, int ExpiresIn);
