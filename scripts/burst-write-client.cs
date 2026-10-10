using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;

public static class BurstWriteClient
{
    private sealed class Result
    {
        public int Index;
        public int Status;
        public double ClientMs;
        public double QueueMs;
        public double ServerMs;
        public string Error;
    }

    private static double HeaderMs(HttpResponseMessage response, string header)
    {
        IEnumerable<string> values;
        if (!response.Headers.TryGetValues(header, out values)) return double.NaN;
        foreach (var value in values) {
            double parsed;
            return double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out parsed) ? parsed : double.NaN;
        }
        return double.NaN;
    }

    private static double Pct(double[] sorted, double fraction)
    {
        return sorted[Math.Max(0, (int)Math.Ceiling(fraction * sorted.Length) - 1)];
    }

    public static string Run(string baseUrl, string token, string itemPrefix, int count)
    {
        var results = new Result[count];
        var stopwatch = Stopwatch.StartNew();
        using (var client = new HttpClient(new HttpClientHandler {
            MaxConnectionsPerServer = count, UseProxy = false
        })) {
            client.DefaultRequestHeaders.TryAddWithoutValidation("Authorization", "Bearer " + token);
            client.Timeout = TimeSpan.FromSeconds(30);
            var tasks = new Task[count];
            for (int i = 0; i < count; i++) {
                int index = i;
                tasks[i] = Task.Run(async () => {
                    var timer = Stopwatch.StartNew();
                    try {
                        var body = "{\"item_id\":\"" + itemPrefix + "_" + index.ToString(CultureInfo.InvariantCulture) + "\",\"quantity\":5}";
                        using (var request = new HttpRequestMessage(HttpMethod.Post, baseUrl + "/players/me/inventory/item")) {
                            request.Content = new StringContent(body, Encoding.UTF8, "application/json");
                            using (var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false)) {
                                using (var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false)) {
                                    var buffer = new byte[4096];
                                    while (await stream.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false) > 0) { }
                                }
                                timer.Stop();
                                results[index] = new Result {
                                    Index = index,
                                    Status = (int)response.StatusCode,
                                    ClientMs = timer.Elapsed.TotalMilliseconds,
                                    QueueMs = HeaderMs(response, "X-Business-Queue-Ms"),
                                    ServerMs = HeaderMs(response, "X-Response-Time-Ms")
                                };
                            }
                        }
                    } catch (Exception ex) {
                        timer.Stop();
                        results[index] = new Result { Index = index, Status = 0, ClientMs = timer.Elapsed.TotalMilliseconds, Error = ex.GetType().Name + ":" + ex.Message };
                    }
                });
            }
            Task.WaitAll(tasks);
        }
        stopwatch.Stop();
        int ok = 0;
        var bad = new List<string>();
        var queueList = new List<double>();
        var clientList = new List<double>();
        for (int i = 0; i < results.Length; i++) {
            var r = results[i];
            if (r.Status >= 200 && r.Status < 300) ok++;
            else bad.Add(string.Format(CultureInfo.InvariantCulture, "BAD i={0} status={1} err={2}", r.Index, r.Status, r.Error));
            if (!double.IsNaN(r.QueueMs)) queueList.Add(r.QueueMs);
            clientList.Add(r.ClientMs);
        }
        var queue = queueList.ToArray();
        var clientArr = clientList.ToArray();
        Array.Sort(queue);
        Array.Sort(clientArr);
        var lines = new List<string> {
            string.Format(CultureInfo.InvariantCulture, "sent={0} ok={1} bad={2} elapsed={3:F3}s", count, ok, results.Length - ok, stopwatch.Elapsed.TotalSeconds),
            string.Format(CultureInfo.InvariantCulture, "queue_header_present={0} queue_p50={1:F3} queue_p95={2:F3} queue_max={3:F3}", queue.Length, queue.Length > 0 ? Pct(queue, .5) : double.NaN, queue.Length > 0 ? Pct(queue, .95) : double.NaN, queue.Length > 0 ? queue[queue.Length - 1] : double.NaN),
            string.Format(CultureInfo.InvariantCulture, "client_p50={0:F3} client_p95={1:F3} client_max={2:F3}", Pct(clientArr, .5), Pct(clientArr, .95), clientArr[clientArr.Length - 1])
        };
        for (int i = 0; i < bad.Count && i < 20; i++) lines.Add(bad[i]);
        return string.Join(Environment.NewLine, lines);
    }

    public static string RunMixed(string baseUrl, string[] tokens, string itemPrefix, int count)
    {
        var results = new Result[count];
        var stopwatch = Stopwatch.StartNew();
        using (var client = new HttpClient(new HttpClientHandler {
            MaxConnectionsPerServer = count, UseProxy = false
        })) {
            client.Timeout = TimeSpan.FromSeconds(30);
            var tasks = new Task[count];
            for (int i = 0; i < count; i++) {
                int index = i;
                string token = tokens[index % tokens.Length];
                tasks[i] = Task.Run(async () => {
                    var timer = Stopwatch.StartNew();
                    try {
                        var body = "{\"item_id\":\"" + itemPrefix + "_" + index.ToString(CultureInfo.InvariantCulture) + "\",\"quantity\":5}";
                        using (var request = new HttpRequestMessage(HttpMethod.Post, baseUrl + "/players/me/inventory/item")) {
                            request.Headers.TryAddWithoutValidation("Authorization", "Bearer " + token);
                            request.Content = new StringContent(body, Encoding.UTF8, "application/json");
                            using (var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false)) {
                                using (var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false)) {
                                    var buffer = new byte[4096];
                                    while (await stream.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false) > 0) { }
                                }
                                timer.Stop();
                                results[index] = new Result {
                                    Index = index,
                                    Status = (int)response.StatusCode,
                                    ClientMs = timer.Elapsed.TotalMilliseconds,
                                    QueueMs = HeaderMs(response, "X-Business-Queue-Ms"),
                                    ServerMs = HeaderMs(response, "X-Response-Time-Ms")
                                };
                            }
                        }
                    } catch (Exception ex) {
                        timer.Stop();
                        results[index] = new Result { Index = index, Status = 0, ClientMs = timer.Elapsed.TotalMilliseconds, Error = ex.GetType().Name + ":" + ex.Message };
                    }
                });
            }
            Task.WaitAll(tasks);
        }
        stopwatch.Stop();
        int ok = 0;
        var bad = new List<string>();
        var queueList = new List<double>();
        var clientList = new List<double>();
        var perTokenQueue = new Dictionary<int, List<double>>();
        for (int i = 0; i < tokens.Length; i++) perTokenQueue[i] = new List<double>();
        for (int i = 0; i < results.Length; i++) {
            var r = results[i];
            if (r.Status >= 200 && r.Status < 300) ok++;
            else bad.Add(string.Format(CultureInfo.InvariantCulture, "BAD i={0} status={1} err={2}", r.Index, r.Status, r.Error));
            if (!double.IsNaN(r.QueueMs)) { queueList.Add(r.QueueMs); perTokenQueue[r.Index % tokens.Length].Add(r.QueueMs); }
            clientList.Add(r.ClientMs);
        }
        var queue = queueList.ToArray();
        var clientArr = clientList.ToArray();
        Array.Sort(queue);
        Array.Sort(clientArr);
        var lines = new List<string> {
            string.Format(CultureInfo.InvariantCulture, "tokens={0} sent={1} ok={2} bad={3} elapsed={4:F3}s", tokens.Length, count, ok, results.Length - ok, stopwatch.Elapsed.TotalSeconds),
            string.Format(CultureInfo.InvariantCulture, "queue_header_present={0} queue_p50={1:F3} queue_p95={2:F3} queue_max={3:F3}", queue.Length, queue.Length > 0 ? Pct(queue, .5) : double.NaN, queue.Length > 0 ? Pct(queue, .95) : double.NaN, queue.Length > 0 ? queue[queue.Length - 1] : double.NaN),
            string.Format(CultureInfo.InvariantCulture, "client_p50={0:F3} client_p95={1:F3} client_max={2:F3}", Pct(clientArr, .5), Pct(clientArr, .95), clientArr[clientArr.Length - 1])
        };
        for (int i = 0; i < tokens.Length; i++) {
            var q = perTokenQueue[i].ToArray();
            Array.Sort(q);
            lines.Add(string.Format(CultureInfo.InvariantCulture, "token[{0}] n={1} queue_p50={2:F3} queue_p95={3:F3} queue_max={4:F3}", i, q.Length, q.Length > 0 ? Pct(q, .5) : double.NaN, q.Length > 0 ? Pct(q, .95) : double.NaN, q.Length > 0 ? q[q.Length - 1] : double.NaN));
        }
        for (int i = 0; i < bad.Count && i < 20; i++) lines.Add(bad[i]);
        return string.Join(Environment.NewLine, lines);
    }
}
