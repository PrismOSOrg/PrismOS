namespace PrismOS.Domain.Packages;

public sealed record PackageRecord(string Id, string Version, string Description,
    string Path, long Size, string Sha256, string? UploadedBy);
