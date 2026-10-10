using PrismOS.Infrastructure;

namespace PrismOS.Web;

public sealed record BackendSettings(InfrastructureOptions Infrastructure, int MaxUploadBytes)
{
    public static BackendSettings Load(IConfiguration configuration, string contentRootPath)
    {
        IConfigurationSection repository = configuration.GetSection("Repository");
        string packagesPath = ResolvePath(contentRootPath,
            repository["PackagesPath"] ?? "Packages");
        int maxUploadBytes = repository.GetValue("MaxUploadBytes", 32 * 1024 * 1024);
        long maxRepositoryBytes = repository.GetValue("MaxRepositoryBytes", 512L * 1024 * 1024);

        string dataDirectory = ResolvePath(contentRootPath,
            configuration["Security:DataDirectory"] ?? "Data");
        string databasePath = ResolvePath(contentRootPath,
            configuration["Security:DatabasePath"] ?? Path.Combine(dataDirectory, "prismrepo.db"));

        return new BackendSettings(
            new InfrastructureOptions(
                packagesPath,
                repository.GetValue("ChunkSize", 4096),
                repository.GetValue("MaxCatalogBytes", 6144),
                maxUploadBytes,
                maxRepositoryBytes,
                databasePath,
                Path.Combine(dataDirectory, "accounts.json")),
            maxUploadBytes);
    }

    private static string ResolvePath(string contentRootPath, string path) =>
        Path.IsPathRooted(path) ? path : Path.Combine(contentRootPath, path);
}
