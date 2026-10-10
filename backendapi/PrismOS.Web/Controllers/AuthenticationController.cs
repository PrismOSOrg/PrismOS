using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.RateLimiting;
using PrismOS.Application.Accounts;
using PrismOS.Web.Infrastructure;
using PrismOS.Web.Models;

namespace PrismOS.Web.Controllers;

[Route("api/v1/auth")]
public sealed class AuthenticationController(IAccountStore accounts) : ControllerBase
{
    [HttpPost("register")]
    [RequestSizeLimit(16 * 1024)]
    [EnableRateLimiting("auth")]
    public async Task<IActionResult> Register([FromBody] CredentialsRequest? request)
    {
        if (request?.Username is null || request.Password is null)
            return BadRequest(new { error = "username and password are required" });
        var result = await accounts.RegisterAsync(request.Username, request.Password,
            HttpContext.RequestAborted);
        if (result.Token is null) return BadRequest(new { error = result.Error });
        return StatusCode(StatusCodes.Status201Created,
            new AuthTokenResponse(result.Token, "Bearer",
                request.Username.Trim().ToLowerInvariant(), 12 * 60 * 60));
    }

    [HttpPost("login")]
    [RequestSizeLimit(16 * 1024)]
    [EnableRateLimiting("auth")]
    public async Task<IActionResult> Login([FromBody] CredentialsRequest? request)
    {
        if (request?.Username is null || request.Password is null)
            return BadRequest(new { error = "username and password are required" });
        string? token = await accounts.LoginAsync(request.Username, request.Password,
            HttpContext.RequestAborted);
        if (token is null) return BearerAuth.Challenge(HttpContext);
        return Ok(new AuthTokenResponse(token, "Bearer",
            request.Username.Trim().ToLowerInvariant(), 12 * 60 * 60));
    }

    [HttpGet("me")]
    public async Task<IActionResult> Me()
    {
        var (_, username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        return username is null ? BearerAuth.Challenge(HttpContext) : Ok(new { username });
    }

    [HttpPost("logout")]
    public async Task<IActionResult> Logout()
    {
        (bool provided, string? username) = await BearerAuth.ResolveAsync(HttpContext, accounts);
        if (!provided || username is null) return BearerAuth.Challenge(HttpContext);
        string authorization = Request.Headers.Authorization.ToString();
        await accounts.LogoutAsync(authorization[7..].Trim(), HttpContext.RequestAborted);
        return NoContent();
    }
}
