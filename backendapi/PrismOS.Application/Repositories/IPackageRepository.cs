using PrismOS.Domain.Packages;

namespace PrismOS.Application.Repositories;

public interface IPackageRepository
{
    ReadOnlyMemory<byte> Catalog { get; }
    int ChunkSize { get; }

    bool TryGetPackage(string id, string version, out PackageRecord? package);
    byte[] CreateManifest(PackageRecord package);
    Task<byte[]?> ReadChunkAsync(PackageRecord package, long offset, int requestedLength,
        CancellationToken cancellationToken);
    Task<PublishPackageResult> PublishAsync(string id, string version, string? description,
        string uploadedBy, Stream input, long? declaredLength, CancellationToken cancellationToken);
}
