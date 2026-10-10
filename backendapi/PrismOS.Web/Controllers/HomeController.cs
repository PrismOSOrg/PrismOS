using Microsoft.AspNetCore.Mvc;

namespace PrismOS.Web.Controllers;

public sealed class HomeController : Controller
{
    public IActionResult Index() => View();
}
