using PrismOS.Application.Accounts;
using PrismOS.Application.Repositories;
using PrismOS.Infrastructure;
using PrismOS.Web;

var builder = WebApplication.CreateBuilder(args);
BackendSettings settings = BackendSettings.Load(builder.Configuration,
    builder.Environment.ContentRootPath);

builder.WebHost.ConfigureKestrel(options =>
    options.Limits.MaxRequestBodySize = settings.MaxUploadBytes);
builder.Services.AddBackendServices();
builder.Services.AddSingleton(settings);

InfrastructureStores stores = await InfrastructureBootstrapper.InitializeAsync(settings.Infrastructure);
builder.Services.AddSingleton<IPackageRepository>(stores.Packages);
builder.Services.AddSingleton<IAccountStore>(stores.Accounts);

var app = builder.Build();
app.UseStaticFiles();
app.UseRateLimiter();
app.MapControllers();
app.MapControllerRoute(
    name: "default",
    pattern: "{controller=Home}/{action=Index}/{id?}");

app.Run();
