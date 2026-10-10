using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Collections.Concurrent;
using PrismOS.Application.Repositories;
using PrismOS.Domain.Packages;

namespace PrismOS.Infrastructure.Repositories;

public sealed class PackageRepository : IPackageRepository
{
    private static readonly Regex SafeSegment = new("^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$", RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly JsonSerializerOptions ManifestJsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };
    private readonly ConcurrentDictionary<(string Id, string Version), PackageRecord> _packages;
    private readonly SemaphoreSlim _publishGate = new(1, 1);
    private readonly string _root;
    private readonly int _chunkSize;
    private readonly int _maxCatalogBytes;
    private readonly int _maxUploadBytes;
    private readonly long _maxRepositoryBytes;
    private byte[] _catalog;

    private PackageRepository(string root, int chunkSize, int maxCatalogBytes,
        int maxUploadBytes, long maxRepositoryBytes,
        Dictionary<(string Id, string Version), PackageRecord> packages)
    {
        _root = root;
        _chunkSize = chunkSize;
        _maxCatalogBytes = maxCatalogBytes;
        _maxUploadBytes = maxUploadBytes;
        _maxRepositoryBytes = maxRepositoryBytes;
        _packages = new ConcurrentDictionary<(string Id, string Version), PackageRecord>(packages);
        _catalog = BuildCatalog(_packages.Values);
        if (_catalog.Length > maxCatalogBytes)
        {
            throw new InvalidDataException($"Catalog is {_catalog.Length} bytes; configured limit is {maxCatalogBytes} bytes.");
        }
    }

    public ReadOnlyMemory<byte> Catalog => Volatile.Read(ref _catalog);

    public static async Task<PackageRepository> LoadAsync(string packagesPath, int chunkSize,
        int maxCatalogBytes, int maxUploadBytes, long maxRepositoryBytes,
        CancellationToken cancellationToken = default)
    {
        if (chunkSize is < 1 or > 4096)
        {
            throw new InvalidDataException("Repository:ChunkSize must be between 1 and 4096 bytes.");
        }
        if (maxCatalogBytes is < 128 or > 8192)
        {
            throw new InvalidDataException("Repository:MaxCatalogBytes must be between 128 and 8192 bytes.");
        }
        if (maxUploadBytes is < 1 or > 256 * 1024 * 1024)
            throw new InvalidDataException("Repository:MaxUploadBytes must be between 1 byte and 256 MiB.");
        if (maxRepositoryBytes < maxUploadBytes || maxRepositoryBytes > 16L * 1024 * 1024 * 1024)
            throw new InvalidDataException("Repository:MaxRepositoryBytes must be at least MaxUploadBytes and at most 16 GiB.");

        string root = Path.GetFullPath(packagesPath);
        Directory.CreateDirectory(root);
        var records = new Dictionary<(string Id, string Version), PackageRecord>();
        foreach (string manifestPath in Directory.EnumerateFiles(root, "manifest.json", SearchOption.AllDirectories))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if ((File.GetAttributes(manifestPath) & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException($"Repository manifests cannot be symlinks: {manifestPath}");
            }

            await using var manifestStream = File.OpenRead(manifestPath);
            PackageManifestSource? source = await JsonSerializer.DeserializeAsync<PackageManifestSource>(
                manifestStream, ManifestJsonOptions, cancellationToken);
            if (source is null || !IsSafeSegment(source.Id) || !IsSafeSegment(source.Version)
                || string.IsNullOrWhiteSpace(source.File) || Path.GetFileName(source.File) != source.File
                || source.File.Contains('/') || source.File.Contains('\\'))
            {
                throw new InvalidDataException($"Invalid package manifest: {manifestPath}");
            }

            string versionDirectory = Path.GetDirectoryName(manifestPath)!;
            string packagePath = Path.GetFullPath(Path.Combine(versionDirectory, source.File));
            string relativePath = Path.GetRelativePath(root, packagePath);
            if (relativePath == ".." || relativePath.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal)
                || Path.IsPathRooted(relativePath) || !File.Exists(packagePath)
                || (File.GetAttributes(packagePath) & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException($"Package file is missing or outside the repository: {manifestPath}");
            }

            var packageInfo = new FileInfo(packagePath);
            string sha256;
            await using (var packageStream = File.OpenRead(packagePath))
            using (var sha = SHA256.Create())
            {
                byte[] hash = await sha.ComputeHashAsync(packageStream, cancellationToken);
                sha256 = Convert.ToHexString(hash).ToLowerInvariant();
            }

            var record = new PackageRecord(source.Id.ToLowerInvariant(), source.Version,
                CleanDescription(source.Description), packagePath, packageInfo.Length, sha256,
                string.IsNullOrWhiteSpace(source.UploadedBy)
                    ? null : CleanDescription(source.UploadedBy));
            if (!records.TryAdd((record.Id, record.Version), record))
            {
                throw new InvalidDataException($"Duplicate package ID/version: {record.Id} {record.Version}");
            }
        }

        return new PackageRepository(root, chunkSize, maxCatalogBytes, maxUploadBytes,
            maxRepositoryBytes, records);
    }

    public bool TryGetPackage(string id, string version, out PackageRecord? package)
    {
        if (!IsSafeSegment(id) || !IsSafeSegment(version))
        {
            package = null;
            return false;
        }
        return _packages.TryGetValue((id.ToLowerInvariant(), version), out package);
    }

    public byte[] CreateManifest(PackageRecord package)
    {
        string downloadPath = $"/api/v1/packages/{package.Id}/{package.Version}/chunk";
        string text = string.Join('\n',
            "PRISMPKG-MANIFEST/1",
            $"id\t{package.Id}",
            $"version\t{package.Version}",
            $"description\t{package.Description}",
            $"size\t{package.Size}",
            $"sha256\t{package.Sha256}",
            $"chunk-size\t{_chunkSize}",
            $"chunk-path\t{downloadPath}",
            $"uploaded-by\t{package.UploadedBy ?? "repository"}",
            string.Empty);
        return Encoding.UTF8.GetBytes(text);
    }

    public async Task<byte[]?> ReadChunkAsync(PackageRecord package, long offset, int requestedLength,
        CancellationToken cancellationToken)
    {
        if (offset < 0 || requestedLength < 1 || requestedLength > _chunkSize
            || offset >= package.Size)
        {
            return null;
        }

        int actualLength = (int)Math.Min(requestedLength, package.Size - offset);
        byte[] chunk = new byte[actualLength];
        await using var stream = new FileStream(package.Path, FileMode.Open, FileAccess.Read,
            FileShare.Read, bufferSize: 4096, useAsync: true);
        stream.Seek(offset, SeekOrigin.Begin);
        await stream.ReadExactlyAsync(chunk, cancellationToken);
        return chunk;
    }

    public async Task<PublishPackageResult> PublishAsync(string id, string version, string? description,
        string uploadedBy, Stream input, long? declaredLength, CancellationToken cancellationToken)
    {
        if (!IsSafeSegment(id) || !IsSafeSegment(version))
            return new PublishPackageResult(PublishPackageStatus.InvalidMetadata, null);
        if (declaredLength is < 0 || declaredLength > _maxUploadBytes)
            return new PublishPackageResult(PublishPackageStatus.TooLarge, null);

        id = id.ToLowerInvariant();
        string cleanDescription = CleanDescription(description);
        await _publishGate.WaitAsync(cancellationToken);
        try
        {
            var key = (id, version);
            if (_packages.ContainsKey(key))
                return new PublishPackageResult(PublishPackageStatus.AlreadyExists, null);

            string versionDirectory = Path.GetFullPath(Path.Combine(_root, id, version));
            string relativeDirectory = Path.GetRelativePath(_root, versionDirectory);
            if (relativeDirectory == ".." || relativeDirectory.StartsWith(".." + Path.DirectorySeparatorChar,
                StringComparison.Ordinal) || Path.IsPathRooted(relativeDirectory))
                return new PublishPackageResult(PublishPackageStatus.InvalidMetadata, null);

            Directory.CreateDirectory(versionDirectory);
            string packagePath = Path.Combine(versionDirectory, "package.prpkg");
            string manifestPath = Path.Combine(versionDirectory, "manifest.json");
            if (File.Exists(packagePath) || File.Exists(manifestPath))
                return new PublishPackageResult(PublishPackageStatus.AlreadyExists, null);

            string temporaryPackage = Path.Combine(versionDirectory, ".upload-" + Guid.NewGuid().ToString("N"));
            string temporaryManifest = Path.Combine(versionDirectory, ".manifest-" + Guid.NewGuid().ToString("N"));
            bool packageMoved = false;
            bool manifestMoved = false;
            bool published = false;
            try
            {
                long totalBytes = 0;
                using IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                byte[] buffer = new byte[8192];
                await using (var output = new FileStream(temporaryPackage, FileMode.CreateNew,
                    FileAccess.Write, FileShare.None, buffer.Length, FileOptions.Asynchronous))
                {
                    int read;
                    while ((read = await input.ReadAsync(buffer, cancellationToken)) != 0)
                    {
                        if (totalBytes + read > _maxUploadBytes)
                            return new PublishPackageResult(PublishPackageStatus.TooLarge, null);
                        totalBytes += read;
                        hash.AppendData(buffer, 0, read);
                        await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
                    }
                    await output.FlushAsync(cancellationToken);
                }

                if (totalBytes == 0)
                    return new PublishPackageResult(PublishPackageStatus.Empty, null);
                if (declaredLength.HasValue && totalBytes != declaredLength.Value)
                    return new PublishPackageResult(PublishPackageStatus.LengthMismatch, null);

                string digest = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
                long repositoryBytes = 0;
                foreach (PackageRecord existing in _packages.Values)
                {
                    if (repositoryBytes > _maxRepositoryBytes - existing.Size)
                        return new PublishPackageResult(PublishPackageStatus.RepositoryFull, null);
                    repositoryBytes += existing.Size;
                }
                if (totalBytes > _maxRepositoryBytes - repositoryBytes)
                    return new PublishPackageResult(PublishPackageStatus.RepositoryFull, null);

                var package = new PackageRecord(id, version, cleanDescription, packagePath,
                    totalBytes, digest, uploadedBy);
                byte[] candidateCatalog = BuildCatalog(_packages.Values.Append(package));
                if (candidateCatalog.Length > _maxCatalogBytes)
                    return new PublishPackageResult(PublishPackageStatus.CatalogFull, null);

                var sourceManifest = new PackageManifestSource(id, version, "package.prpkg",
                    cleanDescription, uploadedBy);
                await using (var manifestStream = new FileStream(temporaryManifest, FileMode.CreateNew,
                    FileAccess.Write, FileShare.None, 4096, FileOptions.Asynchronous))
                {
                    await JsonSerializer.SerializeAsync(manifestStream, sourceManifest,
                        ManifestJsonOptions, cancellationToken);
                    await manifestStream.FlushAsync(cancellationToken);
                }

                File.Move(temporaryPackage, packagePath);
                packageMoved = true;
                File.Move(temporaryManifest, manifestPath);
                manifestMoved = true;
                if (!_packages.TryAdd(key, package))
                    throw new IOException("Package index changed while publishing.");
                Volatile.Write(ref _catalog, candidateCatalog);
                published = true;
                return new PublishPackageResult(PublishPackageStatus.Success, package);
            }
            finally
            {
                if (File.Exists(temporaryPackage)) File.Delete(temporaryPackage);
                if (File.Exists(temporaryManifest)) File.Delete(temporaryManifest);
                if (!published && packageMoved && File.Exists(packagePath)) File.Delete(packagePath);
                if (!published && manifestMoved && File.Exists(manifestPath)) File.Delete(manifestPath);
            }
        }
        finally
        {
            _publishGate.Release();
        }
    }

    public int ChunkSize => _chunkSize;

    private static byte[] BuildCatalog(IEnumerable<PackageRecord> records)
    {
        var builder = new StringBuilder("PRISMPKG-CATALOG/1\n");
        foreach (PackageRecord package in records.OrderBy(item => item.Id, StringComparer.Ordinal)
            .ThenBy(item => item.Version, StringComparer.Ordinal))
        {
            builder.Append(package.Id).Append('\t')
                .Append(package.Version).Append('\t')
                .Append(package.Size).Append('\t')
                .Append(package.Sha256).Append('\t')
                .Append("/api/v1/packages/").Append(package.Id).Append('/')
                .Append(package.Version).Append("/manifest\n");
        }
        return Encoding.UTF8.GetBytes(builder.ToString());
    }

    private static bool IsSafeSegment(string? value) =>
        value is not null && SafeSegment.IsMatch(value);

    private static string CleanDescription(string? description)
    {
        var builder = new StringBuilder(120);
        foreach (char character in description ?? string.Empty)
        {
            if (builder.Length == 120) break;
            builder.Append(char.IsControl(character) ? ' ' : character);
        }
        return builder.ToString().Trim();
    }

    private sealed record PackageManifestSource(string Id, string Version, string File,
        string? Description, string? UploadedBy = null);
}
