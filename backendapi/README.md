# PrismOS Package Repository

A .NET 10 ASP.NET Core HTTP repository for PrismOS packages. It provides public catalog/download APIs plus registered-user accounts, authenticated uploads, favorites, and download analytics. SQLite stores account data and analytics using `Microsoft.Data.Sqlite`; package blobs and their manifests remain filesystem-backed.

## Run locally

From this directory:

```sh
dotnet run --urls http://0.0.0.0:8080
```

The process scans `Packages/` at startup and computes the artifact size and SHA-256 digest. The sample package is available as `hello` version `1.0.0`. Restart the server after changing package files manually. User uploads are indexed immediately.

For QEMU user-mode networking, the guest can usually reach the host at `10.0.2.2:8080`.

## HTTP API

- `GET /healthz` — plain-text `ok` health response.
- `GET /api/v1/catalog` — versioned tab-separated catalog.
- `GET /api/v1/packages/{id}/{version}/manifest` — versioned tab-separated manifest.
- `GET /api/v1/packages/{id}/{version}/chunk?offset=N&length=M` — binary package slice.
- `POST /api/v1/auth/register` — create an account and return a bearer token.
- `POST /api/v1/auth/login` — authenticate and return a bearer token.
- `GET /api/v1/auth/me` and `POST /api/v1/auth/logout` — inspect or revoke the current in-memory session.
- `POST /api/v1/uploads` — authenticated raw binary package upload.
- `GET /api/v1/users/me/favorites`, `PUT/DELETE /api/v1/users/me/favorites/{id}/{version}` — manage the authenticated user's favorites.
- `GET /api/v1/users/me/downloads` — per-user download-start and byte totals (only requests carrying that user's token are attributed).
- `GET /api/v1/stats/downloads` — public aggregate download-start and byte totals by package.

Registration and login accept JSON with `username` and `password`. Usernames are 3–24 ASCII letters, digits, dots, underscores, or hyphens; passwords are 12–128 characters. Passwords are stored as salted PBKDF2-HMAC-SHA256 hashes (310,000 iterations), never as plaintext. Authentication uses opaque 256-bit bearer tokens that expire after 12 hours; only token hashes are stored. Sessions are stored in SQLite and remain valid across restarts until they expire or are revoked. Registration and login are rate-limited to 10 requests per IP per minute.

Uploads require `Authorization: Bearer <token>`, `Content-Type: application/octet-stream`, and `X-Package-Id` plus `X-Package-Version` headers. `X-Package-Description` is optional. The request body is the raw package bytes. A successful upload returns 201 with ID, version, byte size, and SHA-256. Existing versions cannot be overwritten (409); a single upload is limited to 32 MiB and the repository to 512 MiB by default. The catalog size limit still applies. All registered users can upload; there is currently no review/admin role or package deletion endpoint.

Successful chunk requests update aggregate `downloadStarts` and `bytesServed`. A request starting at offset zero counts as a download start; this is not proof that the client completed the entire artifact. If a valid bearer token is supplied to a chunk request, the same counters are attributed to that user. Requests without a token are recorded in aggregate only. Download tracking describes bytes served by the server, not acknowledged bytes at the client.

Catalog format:

```text
PRISMPKG-CATALOG/1
<id>\t<version>\t<size-bytes>\t<lowercase-sha256>\t<manifest-path>
```

Manifest format:

```text
PRISMPKG-MANIFEST/1
id\t<id>
version\t<version>
description\t<description>
size\t<size-bytes>
sha256\t<lowercase-sha256>
chunk-size\t<configured-chunk-size>
chunk-path\t<chunk-endpoint-path>
uploaded-by\t<username-or-repository>
```

Fields are UTF-8. IDs and versions are restricted to 1–32 ASCII letters, digits, dots, underscores, or hyphens, beginning with a letter or digit. Descriptions have tabs and line breaks replaced with spaces and are capped at 120 characters. Catalogs are sorted by ID and version. The server refuses startup if the catalog exceeds the configured 6144-byte limit.

Chunk requests require nonnegative `offset` and `length` from 1 through the configured maximum (4096 bytes). The final chunk may be shorter than requested. Successful chunk responses have `Content-Type: application/octet-stream` and an explicit content length. Invalid parameters return 400, an unknown package version returns 404, and an offset beyond the package returns 416.

On disk, each package version has a `manifest.json` containing `id`, `version`, `description`, `file`, and optional `uploadedBy`. The artifact is stored in the same version directory. Paths are validated to prevent traversal; package identifiers are looked up in the index rather than mapped directly from arbitrary URL segments. SQLite stores users and password hashes, bearer-token hashes, favorites, and aggregate/per-user download statistics in `Data/prismrepo.db` by default. The database uses foreign keys, indexes, and transactional updates; `Data/` is excluded from source control. If `Data/accounts.json` exists from an earlier server version, its users, favorites, and download totals are imported once at startup and the original JSON file is left untouched. Package uploads still use temporary files and atomic renames where possible.

## PrismOS download model and limitations

The OS currently makes synchronous plain-HTTP GET requests into bounded caller buffers, closes the connection after each request, and has no request-header/range support. The chunk endpoint therefore uses query parameters instead of HTTP Range headers. A future downloader can fetch the small catalog and manifest, request fixed-size chunks, append them to a temporary file, and incrementally compute SHA-256. The current OS-side HTTP client is documented with an 8 KiB body limit; keep metadata responses below that limit. Catalog is capped at 6 KiB by default.

This server is development/test infrastructure. It has no TLS, email verification, MFA, admin roles, package-signature verification, or production-grade account recovery. Self-registration permits any account to upload until configured quotas are reached. Do not expose it to an untrusted network or send credentials over plain HTTP. A SHA-256 digest detects byte mismatches but does not authenticate the source. Package binaries are not inspected or verified by the server; the OS must validate format and trust before installation/execution. Treat the package directory as administrator-controlled and immutable while the process is running; restart the process to refresh manually edited files.

## Smoke test

Start the server, then run `sh scripts/smoke-test.sh` and
`sh scripts/auth-smoke-test.sh` from this directory. They check public package
routes, authentication, protected upload, favorites, download tracking, logout,
and invalid ranges. Manual public requests are also available:

```sh
curl -i http://127.0.0.1:8080/healthz
curl -i http://127.0.0.1:8080/api/v1/catalog
curl -i http://127.0.0.1:8080/api/v1/packages/hello/1.0.0/manifest
curl -i 'http://127.0.0.1:8080/api/v1/packages/hello/1.0.0/chunk?offset=0&length=64'
curl -i 'http://127.0.0.1:8080/api/v1/packages/hello/1.0.0/chunk?offset=0&length=99999'
```

The first four requests should return 200; the oversized chunk request should return 400. `Packages/hello/1.0.0/hello.prpkg` is a test fixture only, not an installable OS package.
