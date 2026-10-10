using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;

public static class BurstPostClient
{
    public static string Run(string baseUrl, string token, string path, string jsonBody, int count)
    {
        var statuses = new int[count];
        var errors = new string[count];
        var stopwatch = Stopwatch.StartNew();
        using (var client = new HttpClient(new HttpClientHandler { MaxConnectionsPerServer = count, UseProxy = false })) {
            client.Timeout = TimeSpan.FromSeconds(30);
            var tasks = new Task[count];
            for (int i = 0; i < count; i++) {
                int index = i;
                tasks[i] = Task.Run(async () => {
                    try {
                        using (var request = new HttpRequestMessage(HttpMethod.Post, baseUrl + path)) {
                            request.Headers.TryAddWithoutValidation("Authorization", "Bearer " + token);
                            request.Content = new StringContent(jsonBody, Encoding.UTF8, "application/json");
                            using (var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false)) {
                                using (var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false)) {
                                    var buffer = new byte[4096];
                                    while (await stream.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false) > 0) { }
                                }
                                statuses[index] = (int)response.StatusCode;
                            }
                        }
                    } catch (Exception ex) {
                        statuses[index] = 0;
                        errors[index] = ex.GetType().Name + ":" + ex.Message;
                    }
                });
            }
            Task.WaitAll(tasks);
        }
        stopwatch.Stop();
        var histogram = new Dictionary<int, int>();
        for (int i = 0; i < count; i++) {
            int code = statuses[i];
            histogram.TryGetValue(code, out int n);
            histogram[code] = n + 1;
        }
        var lines = new List<string> { string.Format(CultureInfo.InvariantCulture, "sent={0} elapsed={1:F3}s", count, stopwatch.Elapsed.TotalSeconds) };
        foreach (var key in histogram.Keys) {
            lines.Add(string.Format(CultureInfo.InvariantCulture, "status={0} count={1}", key, histogram[key]));
        }
        for (int i = 0; i < count; i++) if (errors[i] != null) lines.Add("ERR i=" + i + " " + errors[i]);
        return string.Join(Environment.NewLine, lines);
    }
}
