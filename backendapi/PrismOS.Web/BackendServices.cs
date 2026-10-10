using System.Threading.RateLimiting;
using Microsoft.AspNetCore.RateLimiting;

namespace PrismOS.Web;

public static class BackendServices
{
    public static IServiceCollection AddBackendServices(this IServiceCollection services)
    {
        services.AddControllersWithViews();
        services.AddRateLimiter(options =>
        {
            options.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
            options.AddPolicy("auth", context => RateLimitPartition.GetFixedWindowLimiter(
                context.Connection.RemoteIpAddress?.ToString() ?? "unknown",
                _ => new FixedWindowRateLimiterOptions
                {
                    PermitLimit = 10,
                    Window = TimeSpan.FromMinutes(1),
                    QueueLimit = 0,
                    AutoReplenishment = true
                }));
        });
        return services;
    }
}
