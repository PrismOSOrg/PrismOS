namespace PrismOS.Web.Models;

public sealed record CredentialsRequest(string? Username, string? Password);

public sealed record AuthTokenResponse(string AccessToken, string TokenType, string Username, int ExpiresIn);
