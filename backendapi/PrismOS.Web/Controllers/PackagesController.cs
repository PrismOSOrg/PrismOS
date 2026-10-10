using Microsoft.AspNetCore.Mvc;
using PrismOS.Application.Accounts;
using PrismOS.Application.Repositories;
using PrismOS.Domain.Packages;
using PrismOS.Web.Infrastructure;

namespace PrismOS.Web.Controllers;

[Route("api/v1")]
public sealed class PackagesController(
    IPackageRepository packages,
    IAccountStore accounts,
    BackendSettings settings,
    ILogger<PackagesController> logger) : ControllerBase
{
    [HttpGet("catalog")]
    public IActionResult Catalog() =>
        File(packages.Catalog.ToArray(), "text/plain; charset=utf-8");

    [HttpGet("packages/{id}/{version}/manifest")]
    public IActionResult Manifest(string id, string version)
    {
        if (!packages.TryGetPackage(id, version, out PackageRecord? package) || package is null)
            return TextResponse("package version not found\n", StatusCodes.Status404NotFound);
        return File(packages.CreateManifest(package), "text/plain; charset=utf-8");
    }

    [HttpGet("packages/{id}/{version}/chunk")]
    public async Task<IActionResult> Chunk(string id, string version,
        [FromQuery] long? offset, [FromQuery] int? length)
    {
        (bool authProvided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (authProvided && username is null) return BearerAuth.Challenge(HttpContext);
        if (!packages.TryGetPackage(id, version, out PackageRecord? package) || package is null)
            return TextResponse("package version not found\n", StatusCodes.Status404NotFound);
        if (offset is null || length is null || offset < 0 || length < 1 || length > packages.ChunkSize)
            return TextResponse("offset and length are required; length must fit the configured chunk size\n",
                StatusCodes.Status400BadRequest);
        if (offset >= package.Size)
            return TextResponse("chunk offset is outside the package\n", StatusCodes.Status416RangeNotSatisfiable);

        byte[]? bytes = await packages.ReadChunkAsync(package, offset.Value, length.Value,
            HttpContext.RequestAborted);
        if (bytes is null)
            return TextResponse("invalid chunk range\n", StatusCodes.Status400BadRequest);
        try
        {
            await accounts.RecordDownloadChunkAsync(username, package.Id, package.Version,
                offset.Value, bytes.Length, HttpContext.RequestAborted);
        }
        catch (OperationCanceledException) when (HttpContext.RequestAborted.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception)
        {
            logger.LogWarning(exception, "Could not persist download statistics for {PackageId} {Version}",
                package.Id, package.Version);
        }
        return File(bytes, "application/octet-stream");
    }

    [HttpPost("uploads")]
    public async Task<IActionResult> Upload()
    {
        (bool provided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (!provided || username is null) return BearerAuth.Challenge(HttpContext);
        if (!Request.ContentType?.StartsWith("application/octet-stream",
            StringComparison.OrdinalIgnoreCase) ?? true)
            return StatusCode(StatusCodes.Status415UnsupportedMediaType);

        string id = Request.Headers["X-Package-Id"].ToString();
        string version = Request.Headers["X-Package-Version"].ToString();
        string description = Request.Headers["X-Package-Description"].ToString();
        if (string.IsNullOrWhiteSpace(id) || string.IsNullOrWhiteSpace(version))
            return BadRequest(new { error = "X-Package-Id and X-Package-Version are required" });
        if (Request.ContentLength > settings.MaxUploadBytes)
            return StatusCode(StatusCodes.Status413PayloadTooLarge);

        PublishPackageResult result = await packages.PublishAsync(id, version, description,
            username, Request.Body, Request.ContentLength, HttpContext.RequestAborted);
        return result.Status switch
        {
            PublishPackageStatus.Success => StatusCode(StatusCodes.Status201Created, new
            {
                id = result.Package!.Id,
                version = result.Package.Version,
                size = result.Package.Size,
                sha256 = result.Package.Sha256
            }),
            PublishPackageStatus.InvalidMetadata => BadRequest(new { error = "invalid package ID or version" }),
            PublishPackageStatus.AlreadyExists => Conflict(new { error = "that package version already exists" }),
            PublishPackageStatus.TooLarge => StatusCode(StatusCodes.Status413PayloadTooLarge),
            PublishPackageStatus.RepositoryFull => StatusCode(StatusCodes.Status507InsufficientStorage),
            PublishPackageStatus.Empty => BadRequest(new { error = "empty package uploads are not allowed" }),
            PublishPackageStatus.LengthMismatch => BadRequest(new { error = "request body length did not match Content-Length" }),
            PublishPackageStatus.CatalogFull => StatusCode(StatusCodes.Status507InsufficientStorage),
            _ => StatusCode(StatusCodes.Status500InternalServerError)
        };
    }

    [HttpGet("stats/downloads")]
    public async Task<IActionResult> DownloadStats() =>
        Ok(await accounts.GetDownloadStatsAsync(HttpContext.RequestAborted));

    private static ContentResult TextResponse(string content, int statusCode) =>
        new()
        {
            Content = content,
            ContentType = "text/plain; charset=utf-8",
            StatusCode = statusCode
        };
}
