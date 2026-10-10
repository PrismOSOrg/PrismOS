namespace PrismOS.Domain.Packages;

public enum PublishPackageStatus
{
    Success,
    InvalidMetadata,
    AlreadyExists,
    TooLarge,
    RepositoryFull,
    Empty,
    LengthMismatch,
    CatalogFull
}

public sealed record PublishPackageResult(PublishPackageStatus Status, PackageRecord? Package);
