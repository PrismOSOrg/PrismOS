using PrismOS.Application.Accounts;
using PrismOS.Application.Repositories;
using PrismOS.Infrastructure.Accounts;
using PrismOS.Infrastructure.Repositories;

namespace PrismOS.Infrastructure;

public sealed record InfrastructureOptions(
    string PackagesPath,
    int ChunkSize,
    int MaxCatalogBytes,
    int MaxUploadBytes,
    long MaxRepositoryBytes,
    string DatabasePath,
    string? LegacyAccountsPath);

public sealed record InfrastructureStores(IPackageRepository Packages, IAccountStore Accounts);

public static class InfrastructureBootstrapper
{
    public static async Task<InfrastructureStores> InitializeAsync(InfrastructureOptions options,
        CancellationToken cancellationToken = default)
    {
        PackageRepository packages = await PackageRepository.LoadAsync(options.PackagesPath,
            options.ChunkSize, options.MaxCatalogBytes, options.MaxUploadBytes,
            options.MaxRepositoryBytes, cancellationToken);
        AccountStore accounts = await AccountStore.LoadAsync(options.DatabasePath,
            options.LegacyAccountsPath, cancellationToken);
        return new InfrastructureStores(packages, accounts);
    }
}
