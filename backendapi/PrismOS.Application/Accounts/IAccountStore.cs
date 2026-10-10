using PrismOS.Domain.Accounts;

namespace PrismOS.Application.Accounts;

public interface IAccountStore
{
    Task<(string? Token, string? Error)> RegisterAsync(string username, string password,
        CancellationToken cancellationToken);
    Task<string?> LoginAsync(string username, string password, CancellationToken cancellationToken);
    Task<string?> ResolveSessionAsync(string token, CancellationToken cancellationToken);
    Task LogoutAsync(string token, CancellationToken cancellationToken);
    Task<IReadOnlyList<string>?> GetFavoritesAsync(string username, CancellationToken cancellationToken);
    Task<bool?> SetFavoriteAsync(string username, string packageKey, bool favorite,
        CancellationToken cancellationToken);
    Task<IReadOnlyList<UserDownloadStat>?> GetUserDownloadsAsync(string username,
        CancellationToken cancellationToken);
    Task<IReadOnlyList<PackageDownloadStat>> GetDownloadStatsAsync(CancellationToken cancellationToken);
    Task RecordDownloadChunkAsync(string? username, string id, string version,
        long offset, int bytesServed, CancellationToken cancellationToken);
}
