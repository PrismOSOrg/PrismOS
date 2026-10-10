using Microsoft.AspNetCore.Mvc;
using PrismOS.Application.Accounts;
using PrismOS.Application.Repositories;
using PrismOS.Domain.Packages;
using PrismOS.Web.Infrastructure;

namespace PrismOS.Web.Controllers;

[Route("api/v1/users/me")]
public sealed class UsersController(IPackageRepository packages, IAccountStore accounts) : ControllerBase
{
    [HttpGet("favorites")]
    public async Task<IActionResult> Favorites()
    {
        (bool provided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (!provided || username is null) return BearerAuth.Challenge(HttpContext);
        IReadOnlyList<string>? favorites = await accounts.GetFavoritesAsync(username,
            HttpContext.RequestAborted);
        var result = favorites!.Select(key =>
        {
            int separator = key.IndexOf('/');
            string id = key[..separator];
            string version = key[(separator + 1)..];
            return packages.TryGetPackage(id, version, out PackageRecord? package) ? package : null;
        }).Where(package => package is not null)
            .Select(package => new { package!.Id, package.Version, package.Description });
        return Ok(result);
    }

    [HttpPut("favorites/{id}/{version}")]
    public async Task<IActionResult> AddFavorite(string id, string version)
    {
        (bool provided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (!provided || username is null) return BearerAuth.Challenge(HttpContext);
        if (!packages.TryGetPackage(id, version, out _)) return NotFound();
        await accounts.SetFavoriteAsync(username, $"{id.ToLowerInvariant()}/{version}", true,
            HttpContext.RequestAborted);
        return NoContent();
    }

    [HttpDelete("favorites/{id}/{version}")]
    public async Task<IActionResult> RemoveFavorite(string id, string version)
    {
        (bool provided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (!provided || username is null) return BearerAuth.Challenge(HttpContext);
        await accounts.SetFavoriteAsync(username, $"{id.ToLowerInvariant()}/{version}", false,
            HttpContext.RequestAborted);
        return NoContent();
    }

    [HttpGet("downloads")]
    public async Task<IActionResult> Downloads()
    {
        (bool provided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (!provided || username is null) return BearerAuth.Challenge(HttpContext);
        return Ok(await accounts.GetUserDownloadsAsync(username, HttpContext.RequestAborted));
    }
}
