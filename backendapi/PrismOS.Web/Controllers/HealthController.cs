using Microsoft.AspNetCore.Mvc;

namespace PrismOS.Web.Controllers;

[Route("healthz")]
public sealed class HealthController : ControllerBase
{
    [HttpGet]
    public ContentResult Get() => Content("ok\n", "text/plain; charset=utf-8");
}
