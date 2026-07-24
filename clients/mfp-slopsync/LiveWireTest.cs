// =============================================================================
// LiveWireTest — exercises the REAL SlopSync.cs protocol classes (HubClient,
// SlopWire, CborWriter/Reader, MdnsDiscovery, WelcomeInfo) against a live
// SlopDrive-32 device over its actual WebSocket. This is NOT a codec
// self-test (see WireSelfTest.cs, which deliberately re-implements the codec
// to golden-byte-check it) — it links and drives the plugin's own classes,
// unmodified, exactly as SlopSync.cs's SessionAsync does.
//
// Never sends an INTENT frame or any motion command besides the STREAM
// bundles described below. GET-only against the device's HTTP API.
//
// Run:  dotnet run --project clients/mfp-slopsync/LiveWireTest.csproj [ip] [port]
// Exit 0 only if every hard PASS criterion below is met.
// =============================================================================
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net.Http;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using NLog;

internal static class LiveWireTest
{
    private static readonly Logger Log = LogManager.GetCurrentClassLogger();

    private static async Task<int> Main(string[] args)
    {
        // --segments selects the 0x0085 timed-segment path; positional args
        // (ip, port) are read ignoring any --flags.
        bool segments = Array.Exists(args, a => a == "--segments");
        var pos = Array.FindAll(args, a => !a.StartsWith("--"));
        string ip = pos.Length > 0 ? pos[0] : "192.168.1.229";
        int port = pos.Length > 1 ? int.Parse(pos[1]) : 82;
        string baseUrl = $"http://{ip}";

        Console.WriteLine("=============================================================");
        Console.WriteLine($" SlopSync LiveWireTest — target {ip}:{port}  mode={(segments ? "SEGMENTS (0x0085)" : "SAMPLES (0x0084)")}");
        Console.WriteLine("=============================================================");

        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };

        // ---- SAFETY GATE ----------------------------------------------------
        JObject status;
        try
        {
            var body = await http.GetStringAsync($"{baseUrl}/api/status");
            status = JObject.Parse(body);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"ABORT: device unreachable at {baseUrl}/api/status ({ex.Message})");
            return 3;
        }

        bool homed = status.Value<bool?>("homed") ?? false;
        bool estopped = status.Value<bool?>("estopped") ?? false;
        Console.WriteLine($"[gate] homed={homed} estopped={estopped}");
        if (homed)
        {
            Console.WriteLine("ABORT: machine is HOMED — streamed motion would actually move it. Refusing to open a WebSocket.");
            return 3;
        }
        if (estopped)
        {
            Console.WriteLine("ABORT: machine is E-STOPPED. Refusing to open a WebSocket.");
            return 3;
        }
        Console.WriteLine("[gate] PASS — unhomed, not estopped. Streamed motion will be dropped at the firmware HOMED gate (expected & correct).");
        Console.WriteLine();

        // ---- Baseline /api/slopmotion sync counters --------------------------
        var (baseBundles, baseSamples, baseEnqueued, baseDropped) = await ReadSyncCounters(http, baseUrl);
        Console.WriteLine("[baseline] /api/slopmotion sync block:");
        Console.WriteLine($"    bundles={baseBundles} samples={baseSamples} enqueued={baseEnqueued} dropped={baseDropped}");
        Console.WriteLine();

        // ---- Discovery test ----------------------------------------------------
        Console.WriteLine("[discovery] running MdnsDiscovery.DiscoverAsync (2s window)...");
        bool discoveryFound = false;
        try
        {
            var found = await MdnsDiscovery.DiscoverAsync(TimeSpan.FromSeconds(2), Log, CancellationToken.None);
            if (found.Count == 0)
            {
                Console.WriteLine("[discovery] WARN: no devices found (multicast can be flaky on this network/host — not a hard fail).");
            }
            foreach (var d in found)
            {
                Console.WriteLine($"    found: {d.InstanceName}  ip={d.Ip} port={d.Port} fw={d.Fw ?? "(none)"}");
                if (d.Ip == ip && d.Port == port)
                    discoveryFound = true;
            }
            if (found.Count > 0 && !discoveryFound)
                Console.WriteLine($"[discovery] WARN: found device(s), but none matched {ip}:{port}.");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[discovery] WARN: discovery threw: {ex.Message}");
        }
        Console.WriteLine($"[discovery] result: {(discoveryFound ? "FOUND (matches target)" : "NOT CONFIRMED (soft — see warnings above)")}");
        Console.WriteLine();

        // ---- Session test --------------------------------------------------
        var instanceId = new byte[SlopWire.InstanceIdBytes];
        RandomNumberGenerator.Fill(instanceId);

        using var sessionCts = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        var token = sessionCts.Token;

        using var ws = new ClientWebSocket();
        ws.Options.AddSubProtocol(SlopWire.WsSubprotocol);
        var uri = new Uri($"ws://{ip}:{port}/");
        Console.WriteLine($"[ws] connecting to {uri} (subprotocol {SlopWire.WsSubprotocol})...");
        await ws.ConnectAsync(uri, token);
        Console.WriteLine("[ws] connected.");

        var client = new HubClient(ws, instanceId, Log);

        WelcomeInfo welcome;
        double segGranted = double.NaN;
        if (segments)
        {
            Console.WriteLine("[hello] wishing publish on motion-input (0x0084) @ 50 Hz AND motion-segment (0x0085) @ 10 Hz...");
            welcome = await client.HelloAsync("mfp", "LiveWireTest",
                new (ushort ch, double rate)[] { (SlopWire.ChMotionInput, 50.0), (SlopWire.ChMotionSegment, 10.0) }, null, token);
            segGranted = welcome.GrantedPublishRate(SlopWire.ChMotionSegment);
        }
        else
        {
            Console.WriteLine("[hello] wishing publish on motion-input (0x0084) @ 50 Hz...");
            welcome = await client.HelloAsync("mfp", "LiveWireTest", SlopWire.ChMotionInput, 50.0, null, token);
        }
        double granted = welcome.GrantedPublishRate(SlopWire.ChMotionInput);
        Console.WriteLine($"[welcome] session_id={welcome.SessionId} boot_id=0x{welcome.BootId:X8} granted motion-input={granted:F1} Hz (wished 50.0)"
            + (segments ? $" motion-segment={segGranted:F1} Hz (wished 10.0)" : ""));
        Console.WriteLine();

        Console.WriteLine("[subscribe] safety(0x0003) on-change critical + motion(0x0080) @20Hz elevated...");
        await client.SubscribeAsync(new (ushort, double, byte)[]
        {
            (SlopWire.ChSafety, 0.0, SlopWire.PriorityCritical),
            (SlopWire.ChMotion, 20.0, SlopWire.PriorityElevated),
        }, token);

        int nackCount = 0;
        int stateCount = 0;
        var stateByChannel = new Dictionary<ushort, int>();
        var nackLog = new List<(ushort code, ushort channel)>();

        void OnNack(ushort code, ushort channel)
        {
            nackCount++;
            nackLog.Add((code, channel));
            SlopWire.NackNames.TryGetValue(code, out var name);
            Console.WriteLine($"    [recv] NACK {name ?? "UNKNOWN"} (0x{code:X4}) channel=0x{channel:X4}");
        }

        void OnState(ushort channel)
        {
            stateCount++;
            stateByChannel.TryGetValue(channel, out var c);
            stateByChannel[channel] = c + 1;
        }

        var recvTask = client.ReceiveLoopAsync(OnNack, OnState, token);

        // ---- CLOCK sync (mirrors SlopSync.cs's ResyncClock: several
        // exchanges, keep the best-RTT offset) ---------------------------------
        Console.WriteLine("[clock] running 5-exchange sync (keep best RTT)...");
        long bestRtt = long.MaxValue;
        long bestOffset = 0;
        for (int i = 0; i < 5 && !token.IsCancellationRequested; i++)
        {
            var r = await client.ClockExchangeAsync(token);
            if (r == null) continue;
            var (offset, rtt) = r.Value;
            Console.WriteLine($"    exchange {i}: offset={offset} us rtt={rtt} us");
            if (rtt < bestRtt) { bestRtt = rtt; bestOffset = offset; }
        }
        bool haveClock = bestRtt != long.MaxValue;
        if (haveClock)
        {
            client.SetClockOffset(bestOffset);
            Console.WriteLine($"[clock] best: offset={bestOffset} us rtt={bestRtt} us");
        }
        else
        {
            Console.WriteLine("[clock] FAIL: no CLOCK exchange completed.");
        }
        Console.WriteLine();

        // ---- Stream test -----------------------------------------------------
        long sends = 0;
        if (segments)
        {
            // 5 timed segments over ~5 s, alternating target 0.3/0.7, duration
            // 900 ms each, with a PING keepalive every 400 ms of silence between
            // them (segments are sparse — without PING the hub's 600 ms deadman
            // would fire). end_vel alternates sentinel / rest to exercise both.
            Console.WriteLine("[stream] sending 5 segments over ~5 s (target 0.3/0.7, dur 900 ms) with 400 ms PING keepalive...");
            var sw2 = Stopwatch.StartNew();
            double lastSendMs = 0;
            for (int k = 0; k < 5 && !token.IsCancellationRequested; k++)
            {
                double target = (k % 2 == 0) ? 0.3 : 0.7;
                bool sentinel = (k % 2 == 0);                 // even: no end-vel; odd: rest at target
                var seg = new SegmentSample(target, 900, sentinel ? 0.0 : 0.0, sentinel);
                await client.SendSegmentSampleAsync(client.HubNowUs(), seg, token);
                sends++;
                lastSendMs = sw2.Elapsed.TotalMilliseconds;
                Console.WriteLine($"    segment {k}: target={target:F2} dur=900ms end_vel={(sentinel ? "SENTINEL" : "0 (rest)")}");

                // hold ~1 s until the next segment, PINGing when silent > 400 ms
                double until = lastSendMs + 1000;
                while (sw2.Elapsed.TotalMilliseconds < until && !token.IsCancellationRequested)
                {
                    double nowMs = sw2.Elapsed.TotalMilliseconds;
                    if (nowMs - lastSendMs >= 400)
                    {
                        await client.SendPingAsync(token);
                        lastSendMs = nowMs;
                        Console.WriteLine("    ping (keepalive)");
                    }
                    await Task.Delay(50, token);
                }
            }
            Console.WriteLine($"[stream] done: segments={sends} (expected 5)");
        }
        else
        {
            // 5 s @ granted rate, analytic sine + derivative.
            double rateHz = granted > 0 ? granted : 50.0;
            double periodMs = 1000.0 / rateHz;
            const double durationS = 5.0;
            const double freqHz = 0.5;
            const double amp = 0.2;
            const double centre = 0.5;

            Console.WriteLine($"[stream] sending {durationS:F0}s @ {rateHz:F1} Hz (target=0.5+0.2*sin(2*pi*0.5*t))...");
            var sw = Stopwatch.StartNew();
            double nextMs = 0;
            while (sw.Elapsed.TotalSeconds < durationS && !token.IsCancellationRequested)
            {
                double nowMs = sw.Elapsed.TotalMilliseconds;
                if (nowMs < nextMs)
                {
                    int sleep = (int)Math.Max(0, Math.Min(nextMs - nowMs, 5));
                    await Task.Delay(sleep, token);
                    continue;
                }
                nextMs += periodMs;
                if (nextMs < nowMs) nextMs = nowMs + periodMs;

                double t = sw.Elapsed.TotalSeconds;
                double w = 2 * Math.PI * freqHz;
                double target = centre + amp * Math.Sin(w * t);
                double vel = amp * w * Math.Cos(w * t);

                await client.SendStreamSampleAsync(client.HubNowUs(), target, vel, token);
                sends++;
            }
            Console.WriteLine($"[stream] done: sends={sends} (expected ~{(int)Math.Round(rateHz * durationS)})");
        }
        Console.WriteLine();

        // ---- Drain trailing frames, then close cleanly -----------------------
        await Task.Delay(400, CancellationToken.None);
        sessionCts.Cancel();
        try { await recvTask; } catch (OperationCanceledException) { }

        if (ws.State == WebSocketState.Open)
        {
            try { await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "LiveWireTest done", CancellationToken.None); }
            catch { /* best-effort */ }
        }
        Console.WriteLine($"[ws] closed. states_received={stateCount} nacks={nackCount}");
        foreach (var kv in stateByChannel)
            Console.WriteLine($"    STATE channel=0x{kv.Key:X4} count={kv.Value}");
        Console.WriteLine();

        // ---- After counters + diff --------------------------------------------
        var (afterBundles, afterSamples, afterEnqueued, afterDropped) = await ReadSyncCounters(http, baseUrl);
        long dBundles = afterBundles - baseBundles;
        long dSamples = afterSamples - baseSamples;
        long dEnqueued = afterEnqueued - baseEnqueued;
        long dDropped = afterDropped - baseDropped;

        Console.WriteLine("[after] /api/slopmotion sync block:");
        Console.WriteLine($"    bundles={afterBundles} samples={afterSamples} enqueued={afterEnqueued} dropped={afterDropped}");
        Console.WriteLine($"[diff]  bundles={dBundles} samples={dSamples} enqueued={dEnqueued} dropped={dDropped}");
        Console.WriteLine();

        // ---- PASS/FAIL table ----------------------------------------------------
        var checks = new List<(string name, bool pass, string detail)>
        {
            ("granted motion-input rate == 50 Hz", Math.Abs(granted - 50.0) < 0.01, $"granted={granted:F2}"),
            ("CLOCK rtt < 200000 us", haveClock && bestRtt < 200000, haveClock ? $"rtt={bestRtt} us" : "no exchange completed"),
            ("bundles delta == sends (zero wire loss)", dBundles == sends, $"delta={dBundles} sends={sends}"),
            ("samples delta == sends", dSamples == sends, $"delta={dSamples} sends={sends}"),
            ("enqueued delta == 0", dEnqueued == 0, $"delta={dEnqueued}"),
            ("dropped delta == sends (unhomed HOMED-gate drop)", dDropped == sends, $"delta={dDropped} sends={sends}"),
            ("STATE frames received > 0", stateCount > 0, $"count={stateCount}"),
            ("NACKs received == 0", nackCount == 0, $"count={nackCount}"),
        };
        if (segments)
            checks.Insert(1, ("granted motion-segment rate == 10 Hz", Math.Abs(segGranted - 10.0) < 0.01, $"granted={segGranted:F2}"));

        Console.WriteLine("=============================================================");
        Console.WriteLine(" PASS/FAIL");
        Console.WriteLine("=============================================================");
        bool allPass = true;
        foreach (var (name, pass, detail) in checks)
        {
            Console.WriteLine($"  [{(pass ? "PASS" : "FAIL")}] {name}  ({detail})");
            if (!pass) allPass = false;
        }
        Console.WriteLine();
        Console.WriteLine(allPass ? "RESULT: ALL HARD CRITERIA PASS" : "RESULT: FAILURES ABOVE");
        return allPass ? 0 : 1;
    }

    private static async Task<(long bundles, long samples, long enqueued, long dropped)> ReadSyncCounters(HttpClient http, string baseUrl)
    {
        var body = await http.GetStringAsync($"{baseUrl}/api/slopmotion");
        var obj = JObject.Parse(body);
        var sync = obj["sync"];
        long bundles = sync?.Value<long?>("bundles") ?? 0;
        long samples = sync?.Value<long?>("samples") ?? 0;
        long enqueued = sync?.Value<long?>("enqueued") ?? 0;
        long dropped = sync?.Value<long?>("dropped") ?? 0;
        return (bundles, samples, enqueued, dropped);
    }
}
