using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public static class BusinessLoadClient
{
    private sealed class Sample
    {
        public string Route = "";
        public double ClientMs;
        public double ServerMs;
        public double QueueMs;
        public double ProcessingMs;
        public int Status;
    }

    private static double Percentile(double[] sorted, double fraction)
    {
        return sorted[Math.Max(0, (int)Math.Ceiling(fraction * sorted.Length) - 1)];
    }

    private static double HeaderMs(HttpResponseMessage response, string header)
    {
        IEnumerable<string> values;
        double parsed;
        if (!response.Headers.TryGetValues(header, out values))
        {
            return double.NaN;
        }
        foreach (var value in values)
        {
            return double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out parsed)
                ? parsed : double.NaN;
        }
        return double.NaN;
    }

    public static string Run(string baseUrl, string token, int concurrency, int count, int durationSeconds)
    {
        string[] routes = {
            "/players/me", "/friends", "/mail", "/chat/unread", "/players/me/quests",
            "/shop/products", "/gacha/pools", "/friends/requests", "/chat/history/lobby",
            "/quests", "/shop/history", "/gacha/history"
        };
        var samples = new List<Sample>();
        var samplesLock = new object();
        var errors = new Dictionary<string, int>();
        var errorsLock = new object();
        using (var client = new HttpClient(new HttpClientHandler {
            MaxConnectionsPerServer = concurrency, UseProxy = false
        })) {
            client.DefaultRequestHeaders.TryAddWithoutValidation("Authorization", "Bearer " + token);
            client.Timeout = TimeSpan.FromSeconds(10);
            int next = -1;
            int attempted = 0;
            var total = Stopwatch.StartNew();
            var workers = new Task[concurrency];
            for (int workerIndex = 0; workerIndex < concurrency; workerIndex++)
            {
                workers[workerIndex] = Task.Run(async () => {
                int i;
                while ((i = Interlocked.Increment(ref next)) < count && total.Elapsed.TotalSeconds < durationSeconds) {
                    Interlocked.Increment(ref attempted);
                    string path = routes[i % routes.Length];
                    bool post = path == "/shop/history" || path == "/gacha/history";
                    var timer = Stopwatch.StartNew();
                    try {
                        using (var request = new HttpRequestMessage(post ? HttpMethod.Post : HttpMethod.Get, baseUrl + path)) {
                            if (post) request.Content = new StringContent("{\"limit\":20,\"offset\":0}", Encoding.UTF8, "application/json");
                            using (var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false)) {
                                using (var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false)) {
                                    var buffer = new byte[4096];
                                    while (await stream.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false) > 0) { }
                                }
                                timer.Stop();
                                lock (samplesLock)
                                {
                                    samples.Add(new Sample {
                                        Route = (post ? "POST " : "GET ") + path,
                                        ClientMs = timer.Elapsed.TotalMilliseconds,
                                        ServerMs = HeaderMs(response, "X-Response-Time-Ms"),
                                        QueueMs = HeaderMs(response, "X-Business-Queue-Ms"),
                                        ProcessingMs = HeaderMs(response, "X-Server-Processing-Ms"),
                                        Status = (int)response.StatusCode
                                    });
                                }
                            }
                        }
                    } catch (Exception ex) {
                        lock (errorsLock)
                        {
                            var key = ex.GetType().Name;
                            errors[key] = errors.ContainsKey(key) ? errors[key] + 1 : 1;
                        }
                    }
                }
                });
            }
            Task.WaitAll(workers);
            total.Stop();
            var lines = new List<string> {
                string.Format(CultureInfo.InvariantCulture,
                    "concurrency={0} sent={1} responses={2} transport_errors={3} duration={4:F2}s throughput={5:F1}/s",
                    concurrency, attempted, samples.Count,
                    string.Join(",", errors.Select(x => x.Key + "=" + x.Value)),
                    total.Elapsed.TotalSeconds, samples.Count / total.Elapsed.TotalSeconds)
            };
            foreach (var group in samples.GroupBy(x => x.Route).OrderBy(x => x.Key)) {
                var clientMs = group.Select(x => x.ClientMs).OrderBy(x => x).ToArray();
                var serverMs = group.Select(x => x.ServerMs).Where(x => !double.IsNaN(x)).OrderBy(x => x).ToArray();
                var queueMs = group.Select(x => x.QueueMs).Where(x => !double.IsNaN(x)).OrderBy(x => x).ToArray();
                var processingMs = group.Select(x => x.ProcessingMs).Where(x => !double.IsNaN(x)).OrderBy(x => x).ToArray();
                lines.Add(string.Format(CultureInfo.InvariantCulture,
                    "{0}: n={1} bad={2} client_p95={3:F3}ms client_p99={4:F3}ms server_p50={5:F3}ms server_p95={6:F3}ms server_p99={7:F3}ms server_max={8:F3}ms queue_p95={9:F3}ms queue_p99={10:F3}ms processing_p95={11:F3}ms processing_p99={12:F3}ms",
                    group.Key, group.Count(), group.Count(x => x.Status < 200 || x.Status >= 300),
                    Percentile(clientMs, .95), Percentile(clientMs, .99),
                    serverMs.Length > 0 ? Percentile(serverMs, .50) : double.NaN,
                    serverMs.Length > 0 ? Percentile(serverMs, .95) : double.NaN,
                    serverMs.Length > 0 ? Percentile(serverMs, .99) : double.NaN,
                    serverMs.Length > 0 ? serverMs[serverMs.Length - 1] : double.NaN,
                    queueMs.Length > 0 ? Percentile(queueMs, .95) : double.NaN,
                    queueMs.Length > 0 ? Percentile(queueMs, .99) : double.NaN,
                    processingMs.Length > 0 ? Percentile(processingMs, .95) : double.NaN,
                    processingMs.Length > 0 ? Percentile(processingMs, .99) : double.NaN));
            }
            return string.Join(Environment.NewLine, lines);
        }
    }
}
