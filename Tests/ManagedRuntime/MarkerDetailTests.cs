using IMao_WinUI.Models;
using IMao_WinUI.Services;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;

internal static class MarkerDetailTests
{
    public static async Task RunAsync(string testRoot, Action<bool, string> check)
    {
        string directory = Path.Combine(testRoot, "marker-details-" + Guid.NewGuid().ToString("N"));
        string map = Path.Combine(directory, "map");
        string cache = Path.Combine(directory, "cache");
        Directory.CreateDirectory(Path.Combine(map, "states"));
        File.WriteAllText(Path.Combine(map, "states", "state-8.json"), """
            [{"id":"sx·qq","name":"声匣·七丘","location":[
              {"id":"1409977912641277952","stateId":8,"countryId":3,"description":"本地提示：在桥下","floorId":"16","level":"-2/16"},
              {"id":"1409977912641277953","stateId":8,"countryId":3,"description":"另一个点"}]}]
            """);
        var selection = new MarkerSelection { PointId = "1409977912641277952", StateId = 8, CountryId = 3, NameId = "sx_qq" };
        const string pictureUrl = "https://prod-alicdn-community.kurobbs.com/forum/test.png";
        string payload = JsonSerializer.Serialize(new
        {
            code = 200,
            data = new { id = selection.PointId, name = "声匣·七丘 · 攻略", stateId = 8,
                content = new { description = "在线图文攻略", picturesUrl = new[] { pictureUrl, pictureUrl,
                    "http://prod-alicdn-community.kurobbs.com/test.png", "https://kurobbs.com.attacker.test/image.png", "file:///C:/secret.png" },
                    linkVisible = true, link = new { href = "https://www.kurobbs.com/mc/post/test" } } }
        });
        int calls = 0;
        bool requestCorrect = false;
        using var client = new HttpClient(new CallbackHandler(async (request, token) =>
        {
            calls++;
            requestCorrect = request.Method == HttpMethod.Post && request.RequestUri!.AbsoluteUri ==
                "https://api.kurobbs.com/map/core/position/getDetailOnline" && request.Headers.GetValues("source").Single() == "h5" &&
                request.Headers.GetValues("wiki_type").Single() == "10" && request.Headers.GetValues("state_id").Single() == "8" &&
                await request.Content!.ReadAsStringAsync(token) == "id=1409977912641277952" && !request.Headers.Contains("Authorization");
            return JsonResponse(payload);
        }));
        using var service = new MarkerDetailService(map, cache, client);
        MarkerDetail local = await service.GetLocalAsync(selection);
        check(local.Description == "本地提示：在桥下" && local.PointId == selection.PointId && local.FloorId == "16" && local.Level == "-2/16",
            "marker details preserve exact official ID and offline text/layer metadata");
        check(local.SourceUrl.Contains("typeId=sx%C2%B7qq") && local.SourceUrl.Contains("pointId=1409977912641277952"),
            "marker detail source link restores official type aliases without changing point IDs");
        var markers = await service.GetStateMarkersAsync(8);
        check(markers.Count == 2 && markers.All(marker => marker.Scene == "World" && marker.NameId == "sx_qq"),
            "marker catalog includes unselected points and converts official aliases for runtime use");
        var online = await service.GetOnlineAsync(selection, local);
        check(requestCorrect && online.Detail.Description == "在线图文攻略" && online.Detail.PictureUrls.SequenceEqual([pictureUrl]) &&
            online.Detail.GuideUrl == "https://www.kurobbs.com/mc/post/test",
            "public detail request carries exact ID and validated official media without account credentials");
        var cached = await service.GetOnlineAsync(selection, local);
        check(calls == 1 && cached.FromCache && cached.Detail.Description == online.Detail.Description,
            "fresh detail cache reopens without a second HTTP request");

        using var failureClient = new HttpClient(new CallbackHandler((_, _) => throw new HttpRequestException("offline")));
        using var offlineService = new MarkerDetailService(map, cache, failureClient);
        var fallback = await offlineService.GetOnlineAsync(selection, local, refresh: true);
        check(fallback.FromCache && fallback.Detail.Description == online.Detail.Description && fallback.Status.Contains("缓存"),
            "network failure retains cached guides and exposes offline status");

        using var wrongClient = new HttpClient(new CallbackHandler((_, _) => Task.FromResult(JsonResponse(payload.Replace(selection.PointId, "1409977912641277953")))));
        using var wrongService = new MarkerDetailService(map, Path.Combine(cache, "wrong"), wrongClient);
        var wrong = await wrongService.GetOnlineAsync(selection, local);
        check(wrong.Detail == local && !Directory.Exists(Path.Combine(cache, "wrong", "details")),
            "detail response for a different point cannot replace or poison the selected point cache");

        using var pendingClient = new HttpClient(new CallbackHandler(async (_, token) => { await Task.Delay(Timeout.Infinite, token); return JsonResponse(payload); }));
        using var pendingService = new MarkerDetailService(map, Path.Combine(cache, "pending"), pendingClient);
        using var cancellation = new CancellationTokenSource();
        Task<MarkerDetailResult> pending = pendingService.GetOnlineAsync(selection, local, cancellation.Token);
        cancellation.Cancel();
        bool canceled = false;
        try { await pending; } catch (OperationCanceledException) { canceled = true; }
        check(canceled, "switching selected markers cancels pending guide requests rather than returning stale data");

        check(!MarkerDetailService.TryGetPictureUri("https://kurobbs.com.attacker.test/image.png", out _) &&
            !MarkerDetailService.TryGetPictureUri("https://attacker@prod-alicdn-community.kurobbs.com/image.png", out _) &&
            !MarkerDetailService.TryGetExternalUri("javascript:alert(1)", out _) &&
            !MarkerDetailService.TryGetExternalUri("https://127.0.0.1/admin", out _) &&
            !MarkerDetailService.TryGetExternalUri("https://localhost/settings", out _) &&
            !MarkerDetailService.TryGetExternalUri("https://community.kurobbs.com:444/path", out _),
            "guide URLs reject unsafe schemes, impersonated media hosts, credentials, local addresses and custom ports");
        bool rejectedId = false;
        try { await service.GetLocalAsync(selection with { PointId = "../../outside" }); } catch (ArgumentException) { rejectedId = true; }
        check(rejectedId, "point IDs cannot escape the guide cache directory");

        int imageCalls = 0;
        byte[] png = Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aB2kAAAAASUVORK5CYII=");
        using var imageClient = new HttpClient(new CallbackHandler((request, _) =>
        {
            imageCalls++;
            var response = new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(png), RequestMessage = request };
            response.Content.Headers.ContentType = new("image/png");
            return Task.FromResult(response);
        }));
        using var imageService = new MarkerDetailService(map, cache, imageClient);
        string imagePath = await imageService.GetPicturePathAsync(pictureUrl);
        check(File.ReadAllBytes(imagePath).SequenceEqual(png) && await imageService.GetPicturePathAsync(pictureUrl) == imagePath && imageCalls == 1,
            "official guide image downloads are cached locally and reused");
        using var invalidImageClient = new HttpClient(new CallbackHandler((_, _) =>
        {
            var response = new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent("<html>not an image</html>") };
            response.Content.Headers.ContentType = new("image/png");
            return Task.FromResult(response);
        }));
        using var invalidImageService = new MarkerDetailService(map, Path.Combine(cache, "invalid-images"), invalidImageClient);
        bool invalidImage = false;
        try { await invalidImageService.GetPicturePathAsync(pictureUrl); } catch (InvalidDataException) { invalidImage = true; }
        check(invalidImage, "HTML disguised as an image is rejected before it enters the image cache");
    }

    private static HttpResponseMessage JsonResponse(string payload) => new(HttpStatusCode.OK)
    { Content = new StringContent(payload, Encoding.UTF8, "application/json") };

    private sealed class CallbackHandler(Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> callback) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) => callback(request, cancellationToken);
    }
}
