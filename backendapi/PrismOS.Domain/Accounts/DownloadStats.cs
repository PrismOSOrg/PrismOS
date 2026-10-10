namespace PrismOS.Domain.Accounts;

public abstract class DownloadStatBase
{
    public string Id { get; set; } = string.Empty;
    public string Version { get; set; } = string.Empty;
    public long DownloadStarts { get; set; }
    public long BytesServed { get; set; }
    public DateTimeOffset LastRequestedUtc { get; set; }
}

public sealed class PackageDownloadStat : DownloadStatBase
{
}

public sealed class UserDownloadStat : DownloadStatBase
{
}
