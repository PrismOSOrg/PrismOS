using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Data.Sqlite;

namespace PrismPackageRepo;

internal sealed class AccountStore
{
    private const int PasswordIterations = 310_000;
    private const int SchemaVersion = 1;
    private static readonly TimeSpan SessionLifetime = TimeSpan.FromHours(12);
    private static readonly Regex UsernamePattern = new("^[A-Za-z0-9][A-Za-z0-9._-]{2,23}$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly byte[] DummySalt = RandomNumberGenerator.GetBytes(16);
    private readonly string _connectionString;

    private AccountStore(string connectionString) => _connectionString = connectionString;

    public static async Task<AccountStore> LoadAsync(string databasePath, string? legacyJsonPath = null,
        CancellationToken cancellationToken = default)
    {
        string fullDatabasePath = Path.GetFullPath(databasePath);
        Directory.CreateDirectory(Path.GetDirectoryName(fullDatabasePath)!);
        var connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = fullDatabasePath,
            Mode = SqliteOpenMode.ReadWriteCreate,
            Cache = SqliteCacheMode.Shared,
            ForeignKeys = true,
            Pooling = true,
            DefaultTimeout = 30
        }.ToString();
        var store = new AccountStore(connectionString);

        await using SqliteConnection connection = await store.OpenAsync(cancellationToken);
        await using (var command = connection.CreateCommand())
        {
            command.CommandText = "PRAGMA journal_mode=WAL;";
            await command.ExecuteNonQueryAsync(cancellationToken);
        }
        await using (var command = connection.CreateCommand())
        {
            command.CommandText = """
                CREATE TABLE IF NOT EXISTS schema_migrations (
                    version INTEGER PRIMARY KEY,
                    applied_utc INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS users (
                    username TEXT PRIMARY KEY COLLATE NOCASE,
                    salt BLOB NOT NULL CHECK(length(salt) = 16),
                    password_hash BLOB NOT NULL CHECK(length(password_hash) = 32),
                    created_utc INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS sessions (
                    token_hash TEXT PRIMARY KEY,
                    username TEXT NOT NULL REFERENCES users(username) ON DELETE CASCADE,
                    expires_utc INTEGER NOT NULL
                );
                CREATE INDEX IF NOT EXISTS ix_sessions_expiration ON sessions(expires_utc);
                CREATE TABLE IF NOT EXISTS favorites (
                    username TEXT NOT NULL REFERENCES users(username) ON DELETE CASCADE,
                    package_id TEXT NOT NULL,
                    version TEXT NOT NULL,
                    PRIMARY KEY(username, package_id, version)
                );
                CREATE TABLE IF NOT EXISTS download_stats (
                    package_id TEXT NOT NULL,
                    version TEXT NOT NULL,
                    download_starts INTEGER NOT NULL DEFAULT 0,
                    bytes_served INTEGER NOT NULL DEFAULT 0,
                    last_requested_utc INTEGER NOT NULL,
                    PRIMARY KEY(package_id, version)
                );
                CREATE TABLE IF NOT EXISTS user_download_stats (
                    username TEXT NOT NULL REFERENCES users(username) ON DELETE CASCADE,
                    package_id TEXT NOT NULL,
                    version TEXT NOT NULL,
                    download_starts INTEGER NOT NULL DEFAULT 0,
                    bytes_served INTEGER NOT NULL DEFAULT 0,
                    last_requested_utc INTEGER NOT NULL,
                    PRIMARY KEY(username, package_id, version)
                );
                CREATE INDEX IF NOT EXISTS ix_user_download_stats_recent
                    ON user_download_stats(username, last_requested_utc DESC);
                """;
            await command.ExecuteNonQueryAsync(cancellationToken);
        }

        await store.ImportLegacyJsonAsync(legacyJsonPath, cancellationToken);
        await using (var command = connection.CreateCommand())
        {
            command.CommandText = "INSERT OR IGNORE INTO schema_migrations(version, applied_utc) VALUES ($version, $now);";
            command.Parameters.AddWithValue("$version", SchemaVersion);
            command.Parameters.AddWithValue("$now", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
            await command.ExecuteNonQueryAsync(cancellationToken);
        }
        return store;
    }

    public async Task<(string? Token, string? Error)> RegisterAsync(string username, string password,
        CancellationToken cancellationToken)
    {
        username = (username ?? string.Empty).Trim();
        if (!UsernamePattern.IsMatch(username))
            return (null, "Username must be 3–24 letters, digits, dots, underscores, or hyphens.");
        if (password is null || password.Length is < 12 or > 128)
            return (null, "Password must be between 12 and 128 characters.");

        string canonicalUsername = username.ToLowerInvariant();
        byte[] salt = RandomNumberGenerator.GetBytes(16);
        byte[] passwordHash = HashPassword(password, salt);
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = connection.BeginTransaction();
        try
        {
            await using (var command = connection.CreateCommand())
            {
                command.Transaction = transaction;
                command.CommandText = """
                    INSERT INTO users(username, salt, password_hash, created_utc)
                    VALUES ($username, $salt, $password_hash, $created_utc);
                    """;
                command.Parameters.AddWithValue("$username", canonicalUsername);
                command.Parameters.AddWithValue("$salt", salt);
                command.Parameters.AddWithValue("$password_hash", passwordHash);
                command.Parameters.AddWithValue("$created_utc", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
                await command.ExecuteNonQueryAsync(cancellationToken);
            }

            string token = await CreateSessionAsync(connection, transaction, canonicalUsername, cancellationToken);
            await transaction.CommitAsync(cancellationToken);
            return (token, null);
        }
        catch (SqliteException exception) when (exception.SqliteErrorCode == 19)
        {
            await transaction.RollbackAsync(cancellationToken);
            return (null, "Username is already registered.");
        }
    }

    public async Task<string?> LoginAsync(string username, string password,
        CancellationToken cancellationToken)
    {
        if (password is null || password.Length > 128)
        {
            _ = HashPassword(string.Empty, DummySalt);
            return null;
        }
        username = (username ?? string.Empty).Trim().ToLowerInvariant();
        byte[]? salt = null;
        byte[]? expected = null;
        string? canonicalUsername = null;

        await using (SqliteConnection connection = await OpenAsync(cancellationToken))
        await using (var command = connection.CreateCommand())
        {
            command.CommandText = "SELECT username, salt, password_hash FROM users WHERE username = $username;";
            command.Parameters.AddWithValue("$username", username);
            await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
            if (await reader.ReadAsync(cancellationToken))
            {
                canonicalUsername = reader.GetString(0);
                salt = (byte[])reader[1];
                expected = (byte[])reader[2];
            }
        }

        if (salt is null || expected is null)
        {
            _ = HashPassword(password, DummySalt);
            return null;
        }
        byte[] actual = HashPassword(password, salt);
        if (!CryptographicOperations.FixedTimeEquals(actual, expected)) return null;

        await using SqliteConnection sessionConnection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = sessionConnection.BeginTransaction();
        string token = await CreateSessionAsync(sessionConnection, transaction,
            canonicalUsername!, cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return token;
    }

    public async Task<string?> ResolveSessionAsync(string token, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(token)) return null;
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT username FROM sessions
            WHERE token_hash = $token_hash AND expires_utc > $now;
            """;
        command.Parameters.AddWithValue("$token_hash", HashToken(token));
        command.Parameters.AddWithValue("$now", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        return await command.ExecuteScalarAsync(cancellationToken) as string;
    }

    public async Task LogoutAsync(string token, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using var command = connection.CreateCommand();
        command.CommandText = "DELETE FROM sessions WHERE token_hash = $token_hash;";
        command.Parameters.AddWithValue("$token_hash", HashToken(token));
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    public async Task<IReadOnlyList<string>?> GetFavoritesAsync(string username,
        CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT package_id || '/' || version
            FROM favorites WHERE username = $username
            ORDER BY package_id COLLATE BINARY, version COLLATE BINARY;
            """;
        command.Parameters.AddWithValue("$username", username);
        var favorites = new List<string>();
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken)) favorites.Add(reader.GetString(0));
        return favorites;
    }

    public async Task<bool?> SetFavoriteAsync(string username, string packageKey, bool favorite,
        CancellationToken cancellationToken)
    {
        int separator = packageKey.IndexOf('/');
        if (separator <= 0 || separator == packageKey.Length - 1) return false;
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using var command = connection.CreateCommand();
        if (favorite)
        {
            command.CommandText = """
                INSERT OR IGNORE INTO favorites(username, package_id, version)
                SELECT username, $package_id, $version FROM users WHERE username = $username;
                """;
        }
        else
        {
            command.CommandText = """
                DELETE FROM favorites
                WHERE username = $username AND package_id = $package_id AND version = $version;
                """;
        }
        command.Parameters.AddWithValue("$username", username);
        command.Parameters.AddWithValue("$package_id", packageKey[..separator]);
        command.Parameters.AddWithValue("$version", packageKey[(separator + 1)..]);
        int changed = await command.ExecuteNonQueryAsync(cancellationToken);
        return changed != 0;
    }

    public async Task<IReadOnlyList<UserDownloadStat>?> GetUserDownloadsAsync(string username,
        CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT package_id, version, download_starts, bytes_served, last_requested_utc
            FROM user_download_stats WHERE username = $username
            ORDER BY last_requested_utc DESC;
            """;
        command.Parameters.AddWithValue("$username", username);
        var downloads = new List<UserDownloadStat>();
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken))
        {
            downloads.Add(new UserDownloadStat
            {
                Id = reader.GetString(0),
                Version = reader.GetString(1),
                DownloadStarts = reader.GetInt64(2),
                BytesServed = reader.GetInt64(3),
                LastRequestedUtc = DateTimeOffset.FromUnixTimeMilliseconds(reader.GetInt64(4))
            });
        }
        return downloads;
    }

    public async Task<IReadOnlyList<PackageDownloadStat>> GetDownloadStatsAsync(
        CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT package_id, version, download_starts, bytes_served, last_requested_utc
            FROM download_stats
            ORDER BY download_starts DESC, package_id COLLATE BINARY;
            """;
        var downloads = new List<PackageDownloadStat>();
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken))
        {
            downloads.Add(new PackageDownloadStat
            {
                Id = reader.GetString(0),
                Version = reader.GetString(1),
                DownloadStarts = reader.GetInt64(2),
                BytesServed = reader.GetInt64(3),
                LastRequestedUtc = DateTimeOffset.FromUnixTimeMilliseconds(reader.GetInt64(4))
            });
        }
        return downloads;
    }

    public async Task RecordDownloadChunkAsync(string? username, string id, string version,
        long offset, int bytesServed, CancellationToken cancellationToken)
    {
        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        long starts = offset == 0 ? 1 : 0;
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = connection.BeginTransaction();
        await UpsertDownloadAsync(connection, transaction, "download_stats", null,
            id, version, starts, bytesServed, now, cancellationToken);
        if (username is not null)
        {
            await UpsertDownloadAsync(connection, transaction, "user_download_stats", username,
                id, version, starts, bytesServed, now, cancellationToken);
        }
        await transaction.CommitAsync(cancellationToken);
    }

    private async Task ImportLegacyJsonAsync(string? legacyJsonPath, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(legacyJsonPath) || !File.Exists(legacyJsonPath)) return;
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = connection.BeginTransaction();
        await using (var check = connection.CreateCommand())
        {
            check.Transaction = transaction;
            check.CommandText = "SELECT 1 FROM schema_migrations WHERE version = $version;";
            check.Parameters.AddWithValue("$version", 1001);
            if (await check.ExecuteScalarAsync(cancellationToken) is not null) return;
        }

        await using var stream = File.OpenRead(legacyJsonPath);
        LegacySnapshot snapshot = await JsonSerializer.DeserializeAsync<LegacySnapshot>(
            stream, cancellationToken: cancellationToken)
            ?? throw new InvalidDataException("Legacy account data is empty or invalid.");
        foreach (LegacyUser user in snapshot.Users ?? [])
        {
            if (string.IsNullOrWhiteSpace(user.Username) || string.IsNullOrWhiteSpace(user.Salt)
                || string.IsNullOrWhiteSpace(user.PasswordHash))
                throw new InvalidDataException("Legacy account data contains an invalid user.");

            await using (var command = connection.CreateCommand())
            {
                command.Transaction = transaction;
                command.CommandText = """
                    INSERT OR IGNORE INTO users(username, salt, password_hash, created_utc)
                    VALUES ($username, $salt, $password_hash, $created_utc);
                    """;
                command.Parameters.AddWithValue("$username", user.Username.ToLowerInvariant());
                command.Parameters.AddWithValue("$salt", Convert.FromBase64String(user.Salt));
                command.Parameters.AddWithValue("$password_hash", Convert.FromBase64String(user.PasswordHash));
                command.Parameters.AddWithValue("$created_utc", user.CreatedUtc.ToUnixTimeMilliseconds());
                await command.ExecuteNonQueryAsync(cancellationToken);
            }

            foreach (string favorite in user.Favorites ?? [])
            {
                int separator = favorite.IndexOf('/');
                if (separator <= 0 || separator == favorite.Length - 1) continue;
                await using var command = connection.CreateCommand();
                command.Transaction = transaction;
                command.CommandText = """
                    INSERT OR IGNORE INTO favorites(username, package_id, version)
                    VALUES ($username, $package_id, $version);
                    """;
                command.Parameters.AddWithValue("$username", user.Username.ToLowerInvariant());
                command.Parameters.AddWithValue("$package_id", favorite[..separator]);
                command.Parameters.AddWithValue("$version", favorite[(separator + 1)..]);
                await command.ExecuteNonQueryAsync(cancellationToken);
            }

            foreach (LegacyUserDownloadStat item in user.Downloads ?? [])
            {
                await ImportDownloadAsync(connection, transaction, "user_download_stats",
                    user.Username.ToLowerInvariant(), item.Id, item.Version, item.DownloadStarts,
                    item.BytesServed, item.LastRequestedUtc, cancellationToken);
            }
        }

        foreach (LegacyDownloadStat item in snapshot.Downloads ?? [])
        {
            await ImportDownloadAsync(connection, transaction, "download_stats", null,
                item.Id, item.Version, item.DownloadStarts, item.BytesServed,
                item.LastRequestedUtc, cancellationToken);
        }

        await using (var command = connection.CreateCommand())
        {
            command.Transaction = transaction;
            command.CommandText = "INSERT INTO schema_migrations(version, applied_utc) VALUES ($version, $now);";
            command.Parameters.AddWithValue("$version", 1001);
            command.Parameters.AddWithValue("$now", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
            await command.ExecuteNonQueryAsync(cancellationToken);
        }
        await transaction.CommitAsync(cancellationToken);
    }

    private static async Task ImportDownloadAsync(SqliteConnection connection, SqliteTransaction transaction,
        string table, string? username, string id, string version, long starts, long bytes,
        DateTimeOffset lastRequestedUtc, CancellationToken cancellationToken)
    {
        string columns = username is null
            ? "package_id, version, download_starts, bytes_served, last_requested_utc"
            : "username, package_id, version, download_starts, bytes_served, last_requested_utc";
        string values = username is null
            ? "$package_id, $version, $starts, $bytes, $last_requested"
            : "$username, $package_id, $version, $starts, $bytes, $last_requested";
        await using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = $"""
            INSERT OR IGNORE INTO {table}({columns}) VALUES ({values});
            """;
        if (username is not null) command.Parameters.AddWithValue("$username", username);
        command.Parameters.AddWithValue("$package_id", id);
        command.Parameters.AddWithValue("$version", version);
        command.Parameters.AddWithValue("$starts", starts);
        command.Parameters.AddWithValue("$bytes", bytes);
        command.Parameters.AddWithValue("$last_requested", lastRequestedUtc.ToUnixTimeMilliseconds());
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    private static async Task UpsertDownloadAsync(SqliteConnection connection, SqliteTransaction transaction,
        string table, string? username, string id, string version, long starts, int bytes, long now,
        CancellationToken cancellationToken)
    {
        string columns = username is null
            ? "package_id, version, download_starts, bytes_served, last_requested_utc"
            : "username, package_id, version, download_starts, bytes_served, last_requested_utc";
        string values = username is null
            ? "$package_id, $version, $starts, $bytes, $now"
            : "$username, $package_id, $version, $starts, $bytes, $now";
        string conflictKey = username is null
            ? "package_id, version"
            : "username, package_id, version";
        await using var command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = $"""
            INSERT INTO {table}({columns}) VALUES ({values})
            ON CONFLICT({conflictKey}) DO UPDATE SET
                download_starts = download_starts + excluded.download_starts,
                bytes_served = bytes_served + excluded.bytes_served,
                last_requested_utc = excluded.last_requested_utc;
            """;
        if (username is not null) command.Parameters.AddWithValue("$username", username);
        command.Parameters.AddWithValue("$package_id", id);
        command.Parameters.AddWithValue("$version", version);
        command.Parameters.AddWithValue("$starts", starts);
        command.Parameters.AddWithValue("$bytes", bytes);
        command.Parameters.AddWithValue("$now", now);
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    private static async Task<string> CreateSessionAsync(SqliteConnection connection,
        SqliteTransaction transaction, string username, CancellationToken cancellationToken)
    {
        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        await using (var cleanup = connection.CreateCommand())
        {
            cleanup.Transaction = transaction;
            cleanup.CommandText = "DELETE FROM sessions WHERE expires_utc <= $now;";
            cleanup.Parameters.AddWithValue("$now", now);
            await cleanup.ExecuteNonQueryAsync(cancellationToken);
        }

        string token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant();
        await using (var command = connection.CreateCommand())
        {
            command.Transaction = transaction;
            command.CommandText = """
                INSERT INTO sessions(token_hash, username, expires_utc)
                VALUES ($token_hash, $username, $expires_utc);
                """;
            command.Parameters.AddWithValue("$token_hash", HashToken(token));
            command.Parameters.AddWithValue("$username", username);
            command.Parameters.AddWithValue("$expires_utc", now + (long)SessionLifetime.TotalMilliseconds);
            await command.ExecuteNonQueryAsync(cancellationToken);
        }
        return token;
    }

    private async Task<SqliteConnection> OpenAsync(CancellationToken cancellationToken)
    {
        var connection = new SqliteConnection(_connectionString);
        await connection.OpenAsync(cancellationToken);
        return connection;
    }

    private static byte[] HashPassword(string password, byte[] salt) =>
        Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(password), salt,
            PasswordIterations, HashAlgorithmName.SHA256, 32);

    private static string HashToken(string token) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(token)));

    private sealed class LegacySnapshot
    {
        public List<LegacyUser>? Users { get; set; } = [];
        public List<LegacyDownloadStat>? Downloads { get; set; } = [];
    }

    private sealed class LegacyUser
    {
        public string Username { get; set; } = string.Empty;
        public string Salt { get; set; } = string.Empty;
        public string PasswordHash { get; set; } = string.Empty;
        public DateTimeOffset CreatedUtc { get; set; }
        public List<string>? Favorites { get; set; } = [];
        public List<LegacyUserDownloadStat>? Downloads { get; set; } = [];
    }

    private class LegacyDownloadStat
    {
        public string Id { get; set; } = string.Empty;
        public string Version { get; set; } = string.Empty;
        public long DownloadStarts { get; set; }
        public long BytesServed { get; set; }
        public DateTimeOffset LastRequestedUtc { get; set; }
    }

    private sealed class LegacyUserDownloadStat : LegacyDownloadStat
    {
    }
}

internal abstract class DownloadStatBase
{
    public string Id { get; set; } = string.Empty;
    public string Version { get; set; } = string.Empty;
    public long DownloadStarts { get; set; }
    public long BytesServed { get; set; }
    public DateTimeOffset LastRequestedUtc { get; set; }
}

internal sealed class PackageDownloadStat : DownloadStatBase
{
}

internal sealed class UserDownloadStat : DownloadStatBase
{
}