# Package Repository Server Instructions

- This is a read-only ASP.NET Core minimal API for PrismOS packages. Keep OS-side package manager code in the parent project, not here.
- Keep the versioned HTTP contract and tab-separated wire formats documented in `README.md`.
- Keep catalog and manifest responses below the configured size limit. Chunk responses must not exceed `Repository:ChunkSize` (at most 4096 bytes).
- Validate all package identifiers, versions, query ranges, and filesystem paths. Do not add upload or administrative endpoints without an explicit request.
- The server is development/test-only: no TLS, authentication, or signature validation. SHA-256 is integrity metadata, not source authentication.
- Build with `dotnet build`; run the server with `dotnet run --urls http://0.0.0.0:8080`.
