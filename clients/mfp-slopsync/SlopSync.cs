#:name SlopSync
#:version 0.1.0
#:author SlopDrive
#:description Streams a MultiFunPlayer axis to a SlopDrive-32 machine over the native SlopSync protocol (device-shadow + capability negotiation, WebSocket + CBOR).
#:url https://github.com/AtlanticTM

#:reference System.Net.WebSockets.Client
#:reference System.Net.NetworkInformation
#:reference System.Net.Primitives
#:reference System.Net.Sockets
#:reference System.Security.Cryptography

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using MultiFunPlayer.Common;
using MultiFunPlayer.Plugin;
using Newtonsoft.Json;
using NLog;
using Stylet;

// =============================================================================
// SlopSync — MultiFunPlayer plugin: the first external client of the SlopSync
// protocol (docs/slopsync/SPEC.md). It reads an MFP device axis at a fixed rate
// and streams it to a SlopDrive-32 machine as native SlopSync STREAM bundles on
// device channel 0x0084 "motion-input".
//
// This ONE file is the entire plugin (MFP compiles each .cs as a single plugin
// via Roslyn). Every wire number is copied from the registry — the single
// source of truth (docs/slopsync/registry/registry.yaml, mirrored in
// lib/slopsync/.../generated/registry_constants.hpp). The byte-level behaviour
// mirrors tools/slopsync_probe.py, the live-verified Python reference client —
// where the spec and the probe disagree, the probe wins.
// =============================================================================

public class SlopSync : PluginBase
{
    private static readonly Logger Logger = LogManager.GetCurrentClassLogger();

    // ---- Persisted settings (MFP saves/loads any [JsonProperty]) ------------
    private string _address = "192.168.1.229";
    private int _port = 82;
    private int _updateRateHz = 50;
    private string _sourceAxis = "L0";
    private string _pairingPin = "";

    [JsonProperty] public string Address { get => _address; set => SetAndNotify(ref _address, value); }
    [JsonProperty] public int Port { get => _port; set => SetAndNotify(ref _port, value); }
    [JsonProperty] public int UpdateRateHz { get => _updateRateHz; set => SetAndNotify(ref _updateRateHz, value); }
    [JsonProperty] public string SourceAxis { get => _sourceAxis; set => SetAndNotify(ref _sourceAxis, value); }
    [JsonProperty] public string PairingPin { get => _pairingPin; set => SetAndNotify(ref _pairingPin, value); }

    // ---- Live UI state (not persisted) --------------------------------------
    private ConnectionStatus _status = ConnectionStatus.Disconnected;
    private string _statusText;
    private string _deviceInfo;
    private long _sessionId;
    private double _grantedRate;
    private long _clockOffsetUs;
    private long _rttUs;
    private long _bundlesSent;
    private long _nackCount;
    private long _rateLimitedCount;
    private long _statesReceived;
    private double _lastTarget;
    private string _uptime;
    private DiscoveredDevice _selectedDevice;
    private bool _isDiscovering;

    public ConnectionStatus Status
    {
        get => _status;
        set { if (SetAndNotify(ref _status, value)) NotifyOfPropertyChange(nameof(IsEditable)); }
    }
    public string StatusText { get => _statusText; set => SetAndNotify(ref _statusText, value); }
    public string DeviceInfo { get => _deviceInfo; set => SetAndNotify(ref _deviceInfo, value); }
    public long SessionId { get => _sessionId; set => SetAndNotify(ref _sessionId, value); }
    public double GrantedRate { get => _grantedRate; set => SetAndNotify(ref _grantedRate, value); }
    public long ClockOffsetUs { get => _clockOffsetUs; set => SetAndNotify(ref _clockOffsetUs, value); }
    public long RttUs { get => _rttUs; set => SetAndNotify(ref _rttUs, value); }
    public long BundlesSent { get => _bundlesSent; set => SetAndNotify(ref _bundlesSent, value); }
    public long NackCount { get => _nackCount; set => SetAndNotify(ref _nackCount, value); }
    public long RateLimitedCount { get => _rateLimitedCount; set => SetAndNotify(ref _rateLimitedCount, value); }
    public long StatesReceived { get => _statesReceived; set => SetAndNotify(ref _statesReceived, value); }
    public double LastTarget { get => _lastTarget; set => SetAndNotify(ref _lastTarget, value); }
    public string Uptime { get => _uptime; set => SetAndNotify(ref _uptime, value); }

    // True only while fully disconnected — the view binds edit boxes' IsEnabled here.
    public bool IsEditable => Status == ConnectionStatus.Disconnected;

    public ObservableCollection<DiscoveredDevice> DiscoveredDevices { get; } = new();
    public DiscoveredDevice SelectedDevice
    {
        get => _selectedDevice;
        set
        {
            SetAndNotify(ref _selectedDevice, value);
            if (value != null)
            {
                Address = value.Ip;
                Port = value.Port;
            }
        }
    }
    public bool IsDiscovering { get => _isDiscovering; set => SetAndNotify(ref _isDiscovering, value); }

    // ---- Task / connection lifecycle ----------------------------------------
    private Task _task;
    private CancellationTokenSource _cancellationSource;

    // Stable 8-byte identity for this plugin instance (§6.1). Kept for the whole
    // plugin lifetime so a reconnect replaces the old session (DUPLICATE_INSTANCE
    // eviction) rather than piling up ghost sessions on the hub.
    private readonly byte[] _instanceId = NewInstanceId();

    private static byte[] NewInstanceId()
    {
        var b = new byte[SlopWire.InstanceIdBytes];
        System.Security.Cryptography.RandomNumberGenerator.Fill(b);
        return b;
    }

    // Property changes fired from the background task must be raised on the UI
    // thread for WPF; ViewPlugin.cs does it bare, but marshalling keeps binding
    // exceptions off the socket loop. Execute.OnUIThread is a no-op if already
    // on the dispatcher, so it is cheap.
    private void Ui(Action a) => Execute.OnUIThread(a);

    protected override void OnInitialize() { }

    protected override void OnDispose()
    {
        _cancellationSource?.Cancel();
        _cancellationSource?.Dispose();
        _cancellationSource = null;
        _task = null;
    }

    // ---- Toolbar connect/disconnect toggle (bound from the view) ------------
    public void OnConnectClick()
    {
        if (_task == null)
        {
            _cancellationSource = CancellationTokenSource.CreateLinkedTokenSource(CancellationToken);
            var token = _cancellationSource.Token;
            _task = Task.Run(() => RunAsync(token));
        }
        else
        {
            OnDispose();
        }
    }

    // =========================================================================
    // Connection state machine — connect, HELLO/WELCOME, SUBSCRIBE, CLOCK sync,
    // stream loop; auto-reconnect with backoff on unexpected drops.
    // =========================================================================
    private async Task RunAsync(CancellationToken token)
    {
        int[] backoff = { 2000, 5000, 10000 };
        int attempt = 0;

        try
        {
            while (!token.IsCancellationRequested)
            {
                var sessionSw = Stopwatch.StartNew();
                try
                {
                    await SessionAsync(token);
                    // Clean end (should only happen on cancellation) — fall out.
                    if (token.IsCancellationRequested)
                        break;
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex)
                {
                    Logger.Warn(ex, "SlopSync session ended: {0}", ex.Message);
                }

                // A session that ran a good while before dropping earned a fresh
                // backoff — otherwise a drop after hours of streaming still waits
                // out the max 10 s delay like a first-attempt failure.
                if (sessionSw.Elapsed.TotalSeconds > 30)
                    attempt = 0;

                if (token.IsCancellationRequested)
                    break;

                // Unexpected drop → reconnect with backoff.
                int wait = backoff[Math.Min(attempt, backoff.Length - 1)];
                attempt++;
                Ui(() =>
                {
                    Status = ConnectionStatus.Connecting;
                    StatusText = $"Reconnecting in {wait / 1000}s (attempt {attempt})";
                    NotifyOfPropertyChange(nameof(IsEditable));
                });
                try { await Task.Delay(wait, token); }
                catch (OperationCanceledException) { break; }
            }
        }
        catch (OperationCanceledException) { }
        finally
        {
            Ui(() =>
            {
                Status = ConnectionStatus.Disconnected;
                StatusText = null;
                DeviceInfo = null;
                Uptime = null;
                NotifyOfPropertyChange(nameof(IsEditable));
            });
        }
    }

    private async Task SessionAsync(CancellationToken token)
    {
        Ui(() =>
        {
            Status = ConnectionStatus.Connecting;
            StatusText = "Connecting";
            NotifyOfPropertyChange(nameof(IsEditable));
        });

        using var ws = new ClientWebSocket();
        ws.Options.AddSubProtocol(SlopWire.WsSubprotocol);
        var uri = new Uri($"ws://{Address}:{Port}/");
        await ws.ConnectAsync(uri, token);
        Logger.Info("WS connected to {0} (subprotocol {1})", uri, SlopWire.WsSubprotocol);

        var client = new HubClient(ws, _instanceId, Logger);

        // ---- HELLO / WELCOME -------------------------------------------------
        double wishHz = Math.Clamp(UpdateRateHz, 10, 250);
        byte[] token16 = string.IsNullOrEmpty(PairingPin) ? null : Encoding.UTF8.GetBytes(PairingPin);
        var welcome = await client.HelloAsync("mfp", "MultiFunPlayer SlopSync",
            SlopWire.ChMotionInput, wishHz, token16, token);

        double granted = welcome.GrantedPublishRate(SlopWire.ChMotionInput);
        if (double.IsNaN(granted))
            throw new InvalidOperationException("no publish grant for motion-input(0x0084) in WELCOME");

        Ui(() =>
        {
            SessionId = welcome.SessionId;
            GrantedRate = granted;
            DeviceInfo = _selectedDevice != null && !string.IsNullOrEmpty(_selectedDevice.Fw)
                ? $"{_selectedDevice.InstanceName} · fw {_selectedDevice.Fw}"
                : $"boot 0x{welcome.BootId:X8}";
            Status = ConnectionStatus.Connected;
            StatusText = $"Streaming {SourceAxis} @ {granted:F0} Hz";
            NotifyOfPropertyChange(nameof(IsEditable));
        });
        Logger.Info("WELCOME: session={0} boot=0x{1:X8} granted motion-input @ {2:F1} Hz (wished {3:F1})",
            welcome.SessionId, welcome.BootId, granted, wishHz);

        // ---- SUBSCRIBE: safety(0x0003) on-change + motion(0x0080)@20Hz -------
        // Mirrors the probe: safety is never-shed/critical, motion is the live
        // display feed. We only count/observe these; they feed the info panel.
        await client.SubscribeAsync(new (ushort, double, byte)[]
        {
            (SlopWire.ChSafety, 0.0, SlopWire.PriorityCritical),
            (SlopWire.ChMotion, 20.0, SlopWire.PriorityElevated),
        }, token);

        // ---- Receive loop (routes CLOCK replies, NACKs, STATE, PING) --------
        var recvTask = client.ReceiveLoopAsync(OnNack, OnState, token);

        // ---- Initial CLOCK sync ---------------------------------------------
        try
        {
            await ResyncClock(client, token);

            // ---- Stream loop -----------------------------------------------------
            await StreamLoopAsync(client, granted, token);
        }
        finally
        {
            // Best-effort GOODBYE (§6.8) so the hub gets a chance to release
            // this session's source ownership promptly instead of the
            // connection just dying — a courtesy the protocol expects that
            // this plugin never actually sent (BuildGoodbye existed but
            // nothing ever called it). SendFrameAsync only writes (never
            // touches ReceiveAsync), so it's safe to fire alongside recvTask's
            // still-live receive loop below — deliberately NOT pairing this
            // with ws.CloseAsync, which internally consumes reads too and
            // would race that same loop. Its own short-lived token: the
            // caller's token is usually already cancelled by the time we get
            // here — that's the normal reason we're here.
            try
            {
                using var byeCts = new CancellationTokenSource(TimeSpan.FromSeconds(1));
                await client.GoodbyeAsync(SlopWire.GoodbyeNormalClosure, byeCts.Token);
            }
            catch { /* connection may already be gone — nothing more to do */ }
        }

        await recvTask;
    }

    private void OnNack(ushort code, ushort channel)
    {
        Ui(() =>
        {
            NackCount++;
            if (code == SlopWire.NackRateLimited) RateLimitedCount++;
        });
        SlopWire.NackNames.TryGetValue(code, out var name);
        Logger.Warn("NACK {0} (0x{1:X4}) channel=0x{2:X4}", name ?? "UNKNOWN", code, channel);
    }

    private void OnState(ushort channel)
    {
        // Cheap counter — decoding motion/safety payloads for display is
        // optional detail; the count proves the subscription is live.
        Ui(() => StatesReceived++);
    }

    private async Task ResyncClock(HubClient client, CancellationToken token)
    {
        // Several exchanges, keep the best-RTT offset (§7.1). Accuracy of a few
        // ms is fine — this only stamps STREAM t_base.
        long bestRtt = long.MaxValue;
        long bestOffset = 0;
        bool any = false;
        for (int i = 0; i < 5 && !token.IsCancellationRequested; i++)
        {
            var r = await client.ClockExchangeAsync(token);
            if (r == null) continue;
            var (offset, rtt) = r.Value;
            any = true;
            if (rtt < bestRtt) { bestRtt = rtt; bestOffset = offset; }
        }
        if (any)
        {
            client.SetClockOffset(bestOffset);
            Ui(() => { ClockOffsetUs = bestOffset; RttUs = bestRtt; });
            Logger.Info("CLOCK sync: offset ~{0:+#;-#;0} us, rtt ~{1} us", bestOffset, bestRtt);
        }
    }

    private async Task StreamLoopAsync(HubClient client, double rateHz, CancellationToken token)
    {
        double periodMs = 1000.0 / Math.Max(1.0, rateHz);
        var sw = Stopwatch.StartNew();
        double nextMs = 0;
        double prevX = double.NaN;
        double prevMs = 0;
        double velEma = 0;
        const double alpha = 0.5;   // light EMA on the derived velocity

        var connectedAt = DateTime.UtcNow;
        double lastStatsMs = 0;
        double lastResyncMs = 0;
        long localBundles = 0;

        while (!token.IsCancellationRequested)
        {
            double nowMs = sw.Elapsed.TotalMilliseconds;
            if (nowMs < nextMs)
            {
                int sleep = (int)Math.Max(0, Math.Min(nextMs - nowMs, 5));
                await Task.Delay(sleep, token);
                continue;
            }
            nextMs += periodMs;
            if (nextMs < nowMs) nextMs = nowMs + periodMs;   // resync if we fell behind

            // Read the source axis (0..1). ReadProperty is synchronous and
            // thread-safe to call from a background task (see ViewPlugin.cs).
            double x;
            try
            {
                var axis = DeviceAxis.Parse(SourceAxis);
                x = ReadProperty<DeviceAxis, double>("Axis::Value", axis);
            }
            catch (Exception ex)
            {
                Logger.Warn(ex, "axis read failed for '{0}'", SourceAxis);
                x = double.IsNaN(prevX) ? 0.5 : prevX;
            }
            if (double.IsNaN(x)) x = 0.5;

            // Velocity in normalized-strokes/sec = d(pos)/dt, lightly smoothed.
            double vel = 0;
            if (!double.IsNaN(prevX))
            {
                double dt = (nowMs - prevMs) / 1000.0;
                if (dt > 1e-4)
                {
                    double raw = (x - prevX) / dt;
                    velEma = alpha * raw + (1 - alpha) * velEma;
                    vel = velEma;
                }
                else vel = velEma;
            }
            prevX = x;
            prevMs = nowMs;

            await client.SendStreamSampleAsync(client.HubNowUs(), x, vel, token);
            localBundles++;

            // ---- periodic CLOCK resync (~10 s) ------------------------------
            if (nowMs - lastResyncMs >= SlopWire.ClockResyncIntervalMs)
            {
                lastResyncMs = nowMs;
                await ResyncClock(client, token);
            }

            // ---- throttled stats push (no 50 Hz UI churn) -------------------
            if (nowMs - lastStatsMs >= 200)
            {
                lastStatsMs = nowMs;
                long bundlesSnapshot = localBundles;
                double targetSnapshot = x;
                var up = DateTime.UtcNow - connectedAt;
                Ui(() =>
                {
                    BundlesSent = bundlesSnapshot;
                    LastTarget = targetSnapshot;
                    Uptime = $"{(int)up.TotalMinutes:D2}:{up.Seconds:D2}";
                });
            }
        }
    }

    // =========================================================================
    // Discovery — mDNS DNS-SD PTR query for _slopsync._tcp.local.
    // =========================================================================
    public void OnDiscoverClick()
    {
        if (IsDiscovering) return;
        var token = CancellationToken;   // plugin-scoped
        Task.Run(async () =>
        {
            Ui(() => { IsDiscovering = true; DiscoveredDevices.Clear(); });
            try
            {
                var found = await MdnsDiscovery.DiscoverAsync(TimeSpan.FromSeconds(2), Logger, token);
                Ui(() =>
                {
                    foreach (var d in found)
                        DiscoveredDevices.Add(d);
                });
                Logger.Info("SlopSync discovery: {0} device(s) found", found.Count);
            }
            catch (Exception ex)
            {
                Logger.Warn(ex, "discovery failed");
            }
            finally
            {
                Ui(() => IsDiscovering = false);
            }
        });
    }
}

// =============================================================================
// DiscoveredDevice — one mDNS result (bound in the view list).
// =============================================================================
public class DiscoveredDevice
{
    public string InstanceName { get; set; }
    public string Ip { get; set; }
    public int Port { get; set; }
    public string Fw { get; set; }
    public string Display => $"{InstanceName}  —  {Ip}:{Port}" + (string.IsNullOrEmpty(Fw) ? "" : $"  (fw {Fw})");
}

// =============================================================================
// SlopWire — registry constants (docs/slopsync/registry/registry.yaml is the
// single source of truth) + the CBOR / frame / STREAM codec. Every number here
// is copied from the registry and cross-checked against tools/slopsync_probe.py.
// =============================================================================
public static class SlopWire
{
    public const byte ProtocolVersion = 1;                 // registry proto_ver
    public const string WsSubprotocol = "slopsync.v1";     // limits.ws_subprotocol
    public const int InstanceIdBytes = 8;                  // limits.instance_id_bytes
    public const double ClockResyncIntervalMs = 10_000;    // limits.clock_resync_interval_s

    // ---- Frame types (registry frame_types) ---------------------------------
    public const byte FHello = 0x00;
    public const byte FWelcome = 0x01;
    public const byte FPing = 0x03;
    public const byte FPong = 0x04;
    public const byte FClock = 0x05;
    public const byte FSubscribe = 0x06;
    public const byte FGrant = 0x08;
    public const byte FState = 0x0B;
    public const byte FStream = 0x0C;
    public const byte FIntent = 0x0D;
    public const byte FEcho = 0x0E;
    public const byte FNack = 0x10;
    public const byte FGoodbye = 0x11;

    // ---- CBOR map keys (registry cbor_keys) ---------------------------------
    public const int KProtoVer = 1;
    public const int KClientKind = 2;
    public const int KClientName = 3;
    public const int KInstanceId = 4;
    public const int KToken = 5;
    public const int KSessionId = 6;
    public const int KBootId = 7;
    public const int KCatalogEtag = 8;
    public const int KCfgGen = 9;
    public const int KSubscriptions = 10;
    public const int KPublishes = 11;
    public const int KRateHz = 12;
    public const int KPriority = 13;
    public const int KGrantedRateHz = 14;
    public const int KChannelId = 15;
    public const int KCode = 16;
    public const int KDetail = 17;
    public const int KLimits = 22;
    public const int KRoles = 23;
    public const int KDeadmanMs = 24;
    public const int KDeadmanPolicy = 25;
    public const int KNonce = 29;
    public const int KGrants = 35;
    public const int KGrantedPublishes = 36;

    // ---- Priorities (registry Priority) -------------------------------------
    public const byte PriorityBackground = 0;
    public const byte PriorityNormal = 1;
    public const byte PriorityElevated = 2;
    public const byte PriorityCritical = 3;

    // ---- Device channel ids (include/comms/SlopSyncCatalog.h) ---------------
    public const ushort ChSafety = 0x0003;        // channels.safety (STATE, critical)
    public const ushort ChMotion = 0x0080;        // ch::motion (STATE)
    public const ushort ChMotionInput = 0x0084;   // ch::motion_input (STREAM c2h, ≤333 Hz)

    // ---- NACK codes (registry NackCode) — the subset we surface -------------
    public const ushort NackRateLimited = 0x0301;
    public const ushort GoodbyeNormalClosure = 0x0107;   // registry NackCode NORMAL_CLOSURE, reused as a GOODBYE code (§6.8)
    public static readonly IReadOnlyDictionary<ushort, string> NackNames = new Dictionary<ushort, string>
    {
        [0x0000] = "MALFORMED", [0x0001] = "UNSUPPORTED_VERSION", [0x0002] = "FRAME_TOO_LARGE",
        [0x0003] = "PROFILE_VIOLATION", [0x0100] = "BUSY", [0x0101] = "UNAUTHORIZED",
        [0x0102] = "NOT_CONTROLLER", [0x0103] = "PAIRING_REQUIRED", [0x0104] = "PAIRING_DENIED",
        [0x0105] = "SESSION_EVICTED", [0x0106] = "DUPLICATE_INSTANCE", [0x0107] = "NORMAL_CLOSURE",
        [0x0108] = "DEADMAN_TIMEOUT", [0x0200] = "UNKNOWN_CHANNEL", [0x0201] = "ACCESS_DENIED",
        [0x0202] = "CLASS_MISMATCH", [0x0203] = "SUB_LIMIT", [0x0300] = "CONFLICT",
        [0x0301] = "RATE_LIMITED", [0x0302] = "INVALID_VALUE", [0x0303] = "UNSUPPORTED_OP",
        [0x0400] = "ESTOP_ACTIVE", [0x0401] = "NOT_HOMED", [0x0402] = "INTERLOCK",
        [0x0403] = "SOURCE_CONFLICT", [0x0404] = "TAKEOVER_REQUIRED", [0x0405] = "CLEAR_REFUSED",
        [0x0500] = "CHUNK_UNAVAILABLE", [0x0501] = "REASSEMBLY_TIMEOUT", [0x0502] = "ETAG_MISMATCH",
    };

    // ---- Frame header: 8-byte little-endian ---------------------------------
    // [type:u8][flags:u8][channel:u16][seq:u16][len:u16]   (SPEC §5.1)
    public const int HeaderBytes = 8;

    public static byte[] EncodeFrame(byte type, ushort channel, ReadOnlySpan<byte> payload, ushort seq = 0, byte flags = 0)
    {
        var buf = new byte[HeaderBytes + payload.Length];
        buf[0] = type;
        buf[1] = flags;
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(2), channel);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(4), seq);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(6), (ushort)payload.Length);
        payload.CopyTo(buf.AsSpan(HeaderBytes));
        return buf;
    }

    public readonly struct FrameHeader
    {
        public readonly byte Type;
        public readonly byte Flags;
        public readonly ushort Channel;
        public readonly ushort Seq;
        public readonly ushort Len;
        public FrameHeader(byte t, byte f, ushort c, ushort s, ushort l) { Type = t; Flags = f; Channel = c; Seq = s; Len = l; }
    }

    public static bool TryDecodeHeader(ReadOnlySpan<byte> buf, out FrameHeader hdr)
    {
        hdr = default;
        if (buf.Length < HeaderBytes) return false;
        hdr = new FrameHeader(
            buf[0], buf[1],
            BinaryPrimitives.ReadUInt16LittleEndian(buf.Slice(2)),
            BinaryPrimitives.ReadUInt16LittleEndian(buf.Slice(4)),
            BinaryPrimitives.ReadUInt16LittleEndian(buf.Slice(6)));
        return true;
    }

    // ---- HELLO (§6.2) — keys ascending: 1<2<3<4<(5)<11 ----------------------
    // publishes wish: [{12:rate_hz, 15:channel_id}] (§6.2 c2h STREAM wish).
    public static byte[] BuildHello(string clientKind, string clientName, byte[] instanceId,
                                    ushort publishChannel, double publishRateHz, byte[] token16 = null)
    {
        var w = new CborWriter();
        int n = 4 + (token16 != null ? 1 : 0) + 1;   // proto,kind,name,instance (+token) +publishes
        w.WriteMapHeader(n);
        w.WriteUInt(KProtoVer); w.WriteUInt(ProtocolVersion);
        w.WriteUInt(KClientKind); w.WriteTextString(clientKind);
        w.WriteUInt(KClientName); w.WriteTextString(clientName);
        w.WriteUInt(KInstanceId); w.WriteByteString(instanceId);
        if (token16 != null) { w.WriteUInt(KToken); w.WriteByteString(token16); }
        w.WriteUInt(KPublishes);
        w.WriteArrayHeader(1);
        w.WriteMapHeader(2);                          // {12:rate, 15:channel}
        w.WriteUInt(KRateHz); w.WriteFloat32((float)publishRateHz);
        w.WriteUInt(KChannelId); w.WriteUInt(publishChannel);
        return w.ToArray();
    }

    // ---- SUBSCRIBE (§6.6) — {10: [{12:rate,13:prio,15:channel}]} ------------
    public static byte[] BuildSubscribe(IEnumerable<(ushort ch, double rate, byte prio)> wishes)
    {
        var list = wishes.ToList();
        var w = new CborWriter();
        w.WriteMapHeader(1);
        w.WriteUInt(KSubscriptions);
        w.WriteArrayHeader(list.Count);
        foreach (var (ch, rate, prio) in list)
        {
            w.WriteMapHeader(3);                      // keys ascending 12<13<15
            w.WriteUInt(KRateHz); w.WriteFloat32((float)rate);
            w.WriteUInt(KPriority); w.WriteUInt(prio);
            w.WriteUInt(KChannelId); w.WriteUInt(ch);
        }
        return w.ToArray();
    }

    public static byte[] BuildGoodbye(ushort code)
    {
        var w = new CborWriter();
        w.WriteMapHeader(1);
        w.WriteUInt(KCode); w.WriteUInt(code);
        return w.ToArray();
    }

    // ---- CLOCK request (§7.1): raw 4-byte payload = t0 u32 LE ----------------
    public static byte[] BuildClockRequest(uint t0)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, t0);
        return b;
    }

    // ---- STREAM bundle (§5.4) for motion-input (0x0084) ---------------------
    // Layout: [t_base:u32 LE][n:u8][reserved:u8][off:u16 LE]*n
    //         [{target:u16 LE, vel:i16 LE}]*n
    // sample scales (SlopSyncCatalog 0x0084): target *10000, vel *1000.
    public static byte[] BuildStreamBundle(uint tBase, IReadOnlyList<(ushort off, double target, double vel)> samples)
    {
        int n = samples.Count;
        var buf = new byte[6 + n * 2 + n * 4];
        int p = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(p), tBase); p += 4;
        buf[p++] = (byte)n;
        buf[p++] = 0;                                  // reserved
        for (int i = 0; i < n; i++) { BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), samples[i].off); p += 2; }
        for (int i = 0; i < n; i++)
        {
            int rawT = (int)Math.Round(samples[i].target * 10000.0);
            rawT = Math.Clamp(rawT, 0, 65535);
            int rawV = (int)Math.Round(samples[i].vel * 1000.0);
            rawV = Math.Clamp(rawV, -32768, 32767);
            BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), (ushort)rawT); p += 2;
            BinaryPrimitives.WriteInt16LittleEndian(buf.AsSpan(p), (short)rawV); p += 2;
        }
        return buf;
    }

    // Windowed signed u32 difference — util/serial_arithmetic.hpp timeDelta().
    public static long WrapDiff(uint a, uint b)
    {
        long d = (long)(a - b) & 0xFFFFFFFFL;
        if (d >= 0x80000000L) d -= 0x100000000L;
        return d;
    }
}

// =============================================================================
// CborWriter — minimal deterministic CBOR encoder (SPEC §5.3): definite-length
// containers, shortest-form ints, float32-only (0xFA + big-endian binary32),
// map keys written ascending by the caller. Mirrors slopsync_probe.py's encoder.
// =============================================================================
public sealed class CborWriter
{
    private readonly MemoryStream _ms = new();

    private void Head(int major, ulong v)
    {
        int ib0 = major << 5;
        if (v <= 23) { _ms.WriteByte((byte)(ib0 | (int)v)); }
        else if (v <= 0xFF) { _ms.WriteByte((byte)(ib0 | 24)); _ms.WriteByte((byte)v); }
        else if (v <= 0xFFFF) { _ms.WriteByte((byte)(ib0 | 25)); WriteBE(v, 2); }
        else if (v <= 0xFFFFFFFF) { _ms.WriteByte((byte)(ib0 | 26)); WriteBE(v, 4); }
        else { _ms.WriteByte((byte)(ib0 | 27)); WriteBE(v, 8); }
    }

    private void WriteBE(ulong v, int n)
    {
        for (int i = n - 1; i >= 0; i--) _ms.WriteByte((byte)((v >> (8 * i)) & 0xFF));
    }

    public void WriteUInt(long v) => Head(0, (ulong)v);
    public void WriteInt(long v) { if (v >= 0) Head(0, (ulong)v); else Head(1, (ulong)(-1 - v)); }
    public void WriteBool(bool v) => _ms.WriteByte((byte)(v ? 0xF5 : 0xF4));
    public void WriteNull() => _ms.WriteByte(0xF6);

    public void WriteFloat32(float f)
    {
        _ms.WriteByte(0xFA);                    // major 7, ai 26 (float32)
        Span<byte> tmp = stackalloc byte[4];
        BinaryPrimitives.WriteSingleBigEndian(tmp, f);
        _ms.Write(tmp);
    }

    public void WriteTextString(string s)
    {
        var b = Encoding.UTF8.GetBytes(s);
        Head(3, (ulong)b.Length);
        _ms.Write(b, 0, b.Length);
    }

    public void WriteByteString(ReadOnlySpan<byte> b)
    {
        Head(2, (ulong)b.Length);
        _ms.Write(b);
    }

    public void WriteArrayHeader(int count) => Head(4, (ulong)count);
    public void WriteMapHeader(int count) => Head(5, (ulong)count);

    public byte[] ToArray() => _ms.ToArray();
}

// =============================================================================
// CborReader — minimal strict decoder for the same deterministic profile.
// Maps decode to Dictionary<long,object>, arrays to List<object>, ints to long,
// float32 to double, bstr to byte[], tstr to string. Rejects tags & indefinite
// forms. Never throws out of the receive loop's control (callers catch).
// =============================================================================
public sealed class CborReader
{
    private readonly byte[] _b;
    private int _p;
    public CborReader(byte[] b) { _b = b; _p = 0; }

    public object Decode()
    {
        int ib = _b[_p];
        int major = ib >> 5;
        int ai = ib & 0x1F;

        if (major == 7)
        {
            if (ai == 20) { _p++; return false; }
            if (ai == 21) { _p++; return true; }
            if (ai == 22) { _p++; return null; }
            if (ai == 26)
            {
                float f = BinaryPrimitives.ReadSingleBigEndian(_b.AsSpan(_p + 1, 4));
                _p += 5; return (double)f;
            }
            throw new FormatException($"CBOR major7 ai={ai} outside deterministic profile");
        }
        if (major == 6) throw new FormatException("CBOR tags forbidden (§5.3)");

        ulong val;
        if (ai <= 23) { val = (ulong)ai; _p += 1; }
        else if (ai == 24) { val = _b[_p + 1]; _p += 2; }
        else if (ai == 25) { val = ReadBE(2); }
        else if (ai == 26) { val = ReadBE(4); }
        else if (ai == 27) { val = ReadBE(8); }
        else throw new FormatException($"CBOR indefinite/reserved ai={ai} forbidden");

        switch (major)
        {
            case 0: return (long)val;
            case 1: return -1L - (long)val;
            case 2: { var r = _b.AsSpan(_p, (int)val).ToArray(); _p += (int)val; return r; }
            case 3: { var s = Encoding.UTF8.GetString(_b, _p, (int)val); _p += (int)val; return s; }
            case 4:
            {
                var arr = new List<object>((int)val);
                for (ulong i = 0; i < val; i++) arr.Add(Decode());
                return arr;
            }
            case 5:
            {
                var map = new Dictionary<long, object>((int)val);
                for (ulong i = 0; i < val; i++)
                {
                    var k = Decode();
                    var v = Decode();
                    map[Convert.ToInt64(k)] = v;
                }
                return map;
            }
            default: throw new FormatException("unreachable");
        }
    }

    // ai==25/26/27 read the multi-byte length starting at _p+1, then advance _p.
    private ulong ReadBE(int n)
    {
        int start = _p + 1;
        ulong v = 0;
        for (int i = 0; i < n; i++) v = (v << 8) | _b[start + i];
        _p = start + n;
        return v;
    }
}

// =============================================================================
// WelcomeInfo — parsed WELCOME (§6.3).
// =============================================================================
public sealed class WelcomeInfo
{
    public long SessionId;
    public uint BootId;
    public byte[] CatalogEtag;
    public long CfgGen;
    public long Roles;
    public long DeadmanMs;
    public long DeadmanPolicy;
    private readonly List<(ushort ch, double rate)> _grantedPublishes = new();

    public static WelcomeInfo Parse(byte[] payload)
    {
        var map = (Dictionary<long, object>)new CborReader(payload).Decode();
        var w = new WelcomeInfo();
        if (map.TryGetValue(SlopWire.KSessionId, out var sid)) w.SessionId = Convert.ToInt64(sid);
        if (map.TryGetValue(SlopWire.KBootId, out var bid)) w.BootId = (uint)Convert.ToInt64(bid);
        if (map.TryGetValue(SlopWire.KCatalogEtag, out var et) && et is byte[] etb) w.CatalogEtag = etb;
        if (map.TryGetValue(SlopWire.KCfgGen, out var cg)) w.CfgGen = Convert.ToInt64(cg);
        if (map.TryGetValue(SlopWire.KRoles, out var rl)) w.Roles = Convert.ToInt64(rl);
        if (map.TryGetValue(SlopWire.KDeadmanMs, out var dm)) w.DeadmanMs = Convert.ToInt64(dm);
        if (map.TryGetValue(SlopWire.KDeadmanPolicy, out var dp)) w.DeadmanPolicy = Convert.ToInt64(dp);
        if (map.TryGetValue(SlopWire.KGrantedPublishes, out var gp) && gp is List<object> list)
        {
            foreach (var item in list)
            {
                if (item is Dictionary<long, object> e)
                {
                    ushort ch = e.TryGetValue(SlopWire.KChannelId, out var c) ? (ushort)Convert.ToInt64(c) : (ushort)0;
                    double rate = e.TryGetValue(SlopWire.KGrantedRateHz, out var r) ? Convert.ToDouble(r) : 0.0;
                    w._grantedPublishes.Add((ch, rate));
                }
            }
        }
        return w;
    }

    // Granted rate for a publish channel, or NaN if it wasn't granted.
    public double GrantedPublishRate(ushort channel)
    {
        foreach (var (ch, rate) in _grantedPublishes)
            if (ch == channel) return rate;
        return double.NaN;
    }
}

// =============================================================================
// HubClient — one WebSocket session. Owns a send lock (ClientWebSocket allows
// one outstanding send + one outstanding receive), a single receive loop, and
// the CLOCK-offset estimate. STREAM frames carry an incrementing per-channel
// seq (§7.3); control frames use seq 0 like the probe.
// =============================================================================
public sealed class HubClient
{
    private readonly ClientWebSocket _ws;
    private readonly byte[] _instanceId;
    private readonly Logger _log;
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    private volatile int _clockOffset;   // hub_us - client_us (windowed), stored as int32
    private ushort _streamSeq;

    // CLOCK reply plumbing: the receive loop captures t3 at arrival and hands
    // the raw (t0e,t1,t2,t3) to whoever is awaiting an exchange.
    private TaskCompletionSource<(uint t0e, uint t1, uint t2, uint t3)> _pendingClock;

    private Action<ushort, ushort> _onNack;
    private Action<ushort> _onState;

    public HubClient(ClientWebSocket ws, byte[] instanceId, Logger log)
    {
        _ws = ws; _instanceId = instanceId; _log = log;
    }

    public static uint ClientNowUs() =>
        (uint)((Stopwatch.GetTimestamp() * 1_000_000L / Stopwatch.Frequency) & 0xFFFFFFFF);

    public void SetClockOffset(long offset) => _clockOffset = (int)offset;
    public uint HubNowUs() => (uint)((ClientNowUs() + (uint)_clockOffset) & 0xFFFFFFFF);

    private async Task SendFrameAsync(byte type, ushort channel, byte[] payload, ushort seq, CancellationToken token)
    {
        var frame = SlopWire.EncodeFrame(type, channel, payload, seq);
        await _sendLock.WaitAsync(token);
        try
        {
            await _ws.SendAsync(frame, WebSocketMessageType.Binary, true, token);
        }
        finally { _sendLock.Release(); }
    }

    // ---- HELLO / await WELCOME (done inline before the receive loop starts) --
    public async Task<WelcomeInfo> HelloAsync(string kind, string name, ushort publishChannel,
        double publishRateHz, byte[] token16, CancellationToken token)
    {
        var hello = SlopWire.BuildHello(kind, name, _instanceId, publishChannel, publishRateHz, token16);
        await SendFrameAsync(SlopWire.FHello, 0, hello, 0, token);

        using var cts = CancellationTokenSource.CreateLinkedTokenSource(token);
        cts.CancelAfter(TimeSpan.FromSeconds(5));
        while (true)
        {
            var (hdr, payload) = await ReceiveFrameAsync(cts.Token);
            if (hdr.Type == SlopWire.FWelcome)
                return WelcomeInfo.Parse(payload);
            if (hdr.Type == SlopWire.FNack)
            {
                var m = (Dictionary<long, object>)new CborReader(payload).Decode();
                ushort code = m.TryGetValue(SlopWire.KCode, out var c) ? (ushort)Convert.ToInt64(c) : (ushort)0;
                SlopWire.NackNames.TryGetValue(code, out var nm);
                throw new InvalidOperationException($"HELLO refused: NACK {nm ?? "?"} (0x{code:X4})");
            }
            // ignore anything else during the handshake (§4.3 tolerance)
        }
    }

    public async Task SubscribeAsync(IEnumerable<(ushort, double, byte)> wishes, CancellationToken token)
    {
        var payload = SlopWire.BuildSubscribe(wishes);
        await SendFrameAsync(SlopWire.FSubscribe, 0, payload, 0, token);
    }

    // ---- GOODBYE (§6.8) — courtesy teardown, always attempted with its own
    // short-lived token so it still gets a chance to go out even when the
    // caller's own token is already cancelled (the common shutdown case).
    public Task GoodbyeAsync(ushort code, CancellationToken token) =>
        SendFrameAsync(SlopWire.FGoodbye, 0, SlopWire.BuildGoodbye(code), 0, token);

    // ---- CLOCK exchange (§7.1) — routed through the receive loop ------------
    public async Task<(long offset, long rtt)?> ClockExchangeAsync(CancellationToken token)
    {
        var tcs = new TaskCompletionSource<(uint, uint, uint, uint)>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pendingClock = tcs;

        uint t0 = ClientNowUs();
        await SendFrameAsync(SlopWire.FClock, 0, SlopWire.BuildClockRequest(t0), 0, token);

        using var cts = CancellationTokenSource.CreateLinkedTokenSource(token);
        cts.CancelAfter(TimeSpan.FromSeconds(2));
        try
        {
            var reg = cts.Token.Register(() => tcs.TrySetCanceled());
            var (t0e, t1, t2, t3) = await tcs.Task;
            reg.Dispose();
            long offset = (SlopWire.WrapDiff(t1, t0e) + SlopWire.WrapDiff(t2, t3)) / 2;
            long rtt = SlopWire.WrapDiff(t3, t0e) - SlopWire.WrapDiff(t2, t1);
            return (offset, rtt);
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        {
            return null;   // CLOCK timed out; caller keeps its previous offset
        }
        finally { _pendingClock = null; }
    }

    // ---- STREAM sample (§9.2) — single-sample bundle, incrementing seq ------
    public Task SendStreamSampleAsync(uint tBase, double target, double vel, CancellationToken token)
    {
        var payload = SlopWire.BuildStreamBundle(tBase, new (ushort, double, double)[] { (0, target, vel) });
        ushort seq = _streamSeq++;
        return SendFrameAsync(SlopWire.FStream, SlopWire.ChMotionInput, payload, seq, token);
    }

    // ---- Receive loop -------------------------------------------------------
    // Single reader. Routes CLOCK replies to the pending exchange, PING→PONG,
    // NACK/STATE to the plugin callbacks. A malformed frame is logged & skipped,
    // never thrown out of the loop (§4.3 / defensive requirement).
    public async Task ReceiveLoopAsync(Action<ushort, ushort> onNack, Action<ushort> onState, CancellationToken token)
    {
        _onNack = onNack; _onState = onState;
        while (!token.IsCancellationRequested)
        {
            SlopWire.FrameHeader hdr;
            byte[] payload;
            try
            {
                (hdr, payload) = await ReceiveFrameAsync(token);
            }
            catch (OperationCanceledException) { break; }
            catch (WebSocketException wex) { _log.Info("WS closed: {0}", wex.Message); throw; }

            try { Dispatch(hdr, payload, token); }
            catch (Exception ex) { _log.Warn(ex, "dropping malformed frame type=0x{0:X2}", hdr.Type); }
        }
    }

    private void Dispatch(SlopWire.FrameHeader hdr, byte[] payload, CancellationToken token)
    {
        switch (hdr.Type)
        {
            case SlopWire.FClock:
                if (payload.Length >= 12)
                {
                    uint t3 = ClientNowUs();
                    uint t0e = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(0));
                    uint t1 = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4));
                    uint t2 = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(8));
                    _pendingClock?.TrySetResult((t0e, t1, t2, t3));
                }
                break;
            case SlopWire.FPing:
                // §6.5: answer PING with PONG echoing the payload.
                _ = SendFrameAsync(SlopWire.FPong, hdr.Channel, payload, 0, token);
                break;
            case SlopWire.FNack:
            {
                var m = (Dictionary<long, object>)new CborReader(payload).Decode();
                ushort code = m.TryGetValue(SlopWire.KCode, out var c) ? (ushort)Convert.ToInt64(c) : (ushort)0;
                ushort ch = m.TryGetValue(SlopWire.KChannelId, out var cc) ? (ushort)Convert.ToInt64(cc) : hdr.Channel;
                _onNack?.Invoke(code, ch);
                break;
            }
            case SlopWire.FState:
                _onState?.Invoke(hdr.Channel);
                break;
            case SlopWire.FGrant:
            case SlopWire.FEcho:
            case SlopWire.FWelcome:
            case SlopWire.FPong:
            default:
                // Counted-elsewhere or ignorable during steady-state streaming.
                break;
        }
    }

    // Reads exactly one SlopSync frame (one binary WS message). The firmware
    // sends each frame as a single WS message; we accumulate continuation
    // fragments until EndOfMessage, then decode the 8-byte header.
    private async Task<(SlopWire.FrameHeader, byte[])> ReceiveFrameAsync(CancellationToken token)
    {
        var buffer = new byte[2048];
        using var ms = new MemoryStream();
        while (true)
        {
            var result = await _ws.ReceiveAsync(buffer, token);
            if (result.MessageType == WebSocketMessageType.Close)
                throw new WebSocketException("hub closed the connection");
            ms.Write(buffer, 0, result.Count);
            if (result.EndOfMessage) break;
        }
        var data = ms.ToArray();
        if (!SlopWire.TryDecodeHeader(data, out var hdr))
            throw new FormatException($"frame shorter than {SlopWire.HeaderBytes}-byte header ({data.Length} B)");
        int len = Math.Min(hdr.Len, Math.Max(0, data.Length - SlopWire.HeaderBytes));
        var payload = new byte[len];
        Array.Copy(data, SlopWire.HeaderBytes, payload, 0, len);
        return (hdr, payload);
    }
}

// =============================================================================
// MdnsDiscovery — hand-rolled DNS-SD over multicast UDP. Sends a PTR query for
// _slopsync._tcp.local on every up IPv4 interface (unicast-response bit set so
// responders reply to our ephemeral port — avoids fighting for port 5353),
// listens ~2 s, and stitches PTR→SRV→A/TXT into DiscoveredDevice records.
// Every socket op is guarded; a refusing interface is skipped, never fatal.
// =============================================================================
public static class MdnsDiscovery
{
    private const string Service = "_slopsync._tcp.local";   // limits.mdns_service + .local
    private static readonly IPAddress MdnsGroup = IPAddress.Parse("224.0.0.251");
    private const int MdnsPort = 5353;

    public static async Task<List<DiscoveredDevice>> DiscoverAsync(TimeSpan window, Logger log, CancellationToken token)
    {
        var sockets = new List<UdpClient>();
        foreach (var local in UpIPv4Addresses())
        {
            try
            {
                var udp = new UdpClient(AddressFamily.InterNetwork);
                udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                udp.Client.Bind(new IPEndPoint(local, 0));   // ephemeral port; QU responses come back here
                try { udp.JoinMulticastGroup(MdnsGroup, local); } catch { /* some ifaces refuse; unicast still works */ }
                sockets.Add(udp);
            }
            catch (Exception ex) { log.Debug("mDNS: skipping interface {0}: {1}", local, ex.Message); }
        }
        if (sockets.Count == 0)
        {
            // Last resort: a single default-route socket.
            try { var u = new UdpClient(AddressFamily.InterNetwork); u.Client.Bind(new IPEndPoint(IPAddress.Any, 0)); sockets.Add(u); }
            catch (Exception ex) { log.Warn(ex, "mDNS: no usable socket"); return new List<DiscoveredDevice>(); }
        }

        var query = BuildPtrQuery(Service);
        var groupEp = new IPEndPoint(MdnsGroup, MdnsPort);
        foreach (var s in sockets)
        {
            try { await s.SendAsync(query, query.Length, groupEp); }
            catch (Exception ex) { log.Debug("mDNS send failed: {0}", ex.Message); }
        }

        // Accumulate answers across all packets in the window.
        var ptrInstances = new HashSet<string>();
        var srv = new Dictionary<string, (string target, int port)>(StringComparer.OrdinalIgnoreCase);
        var txt = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        var aRecords = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        using var winCts = CancellationTokenSource.CreateLinkedTokenSource(token);
        winCts.CancelAfter(window);
        var recvTasks = sockets.Select(s => ListenAsync(s, ptrInstances, srv, txt, aRecords, log, winCts.Token)).ToArray();
        try { await Task.WhenAll(recvTasks); } catch { /* window elapsed */ }

        foreach (var s in sockets) { try { s.Dispose(); } catch { } }

        // Stitch: for each PTR instance, resolve SRV → port + target; target → A;
        // instance → TXT (fw). Fall back gracefully when a record is missing.
        var results = new List<DiscoveredDevice>();
        var instances = new HashSet<string>(ptrInstances, StringComparer.OrdinalIgnoreCase);
        foreach (var s in srv.Keys) instances.Add(s);   // include SRVs even if the PTR packet was missed
        foreach (var inst in instances)
        {
            string ip = null; int port = 82;
            if (srv.TryGetValue(inst, out var sv))
            {
                port = sv.port;
                if (sv.target != null && aRecords.TryGetValue(sv.target, out var tip)) ip = tip;
            }
            if (ip == null) aRecords.TryGetValue(inst, out ip);
            if (ip == null) continue;   // no address → can't offer it

            string fw = null;
            if (txt.TryGetValue(inst, out var kv))
            {
                kv.TryGetValue("fw", out fw);
                if (fw == null) kv.TryGetValue("version", out fw);
            }
            string label = inst.Replace("." + Service, "").Replace(Service, "").TrimEnd('.');
            if (string.IsNullOrEmpty(label)) label = ip;
            results.Add(new DiscoveredDevice { InstanceName = label, Ip = ip, Port = port, Fw = fw });
        }
        return results;
    }

    private static async Task ListenAsync(UdpClient udp, HashSet<string> ptr,
        Dictionary<string, (string, int)> srv, Dictionary<string, Dictionary<string, string>> txt,
        Dictionary<string, string> a, Logger log, CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            UdpReceiveResult res;
            try { res = await udp.ReceiveAsync(token); }
            catch (OperationCanceledException) { break; }
            catch (Exception) { break; }
            try { ParseResponse(res.Buffer, ptr, srv, txt, a); }
            catch (Exception ex) { log.Debug("mDNS parse error: {0}", ex.Message); }
        }
    }

    private static IEnumerable<IPAddress> UpIPv4Addresses()
    {
        foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (ni.OperationalStatus != OperationalStatus.Up) continue;
            if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                if (ua.Address.AddressFamily == AddressFamily.InterNetwork)
                    yield return ua.Address;
        }
    }

    // ---- DNS wire ------------------------------------------------------------
    private static byte[] BuildPtrQuery(string name)
    {
        using var ms = new MemoryStream();
        Span<byte> hdr = stackalloc byte[12];
        // id=0, flags=0 (standard query), qd=1, others 0
        BinaryPrimitives.WriteUInt16BigEndian(hdr.Slice(0), 0);
        BinaryPrimitives.WriteUInt16BigEndian(hdr.Slice(2), 0);
        BinaryPrimitives.WriteUInt16BigEndian(hdr.Slice(4), 1);
        BinaryPrimitives.WriteUInt16BigEndian(hdr.Slice(6), 0);
        BinaryPrimitives.WriteUInt16BigEndian(hdr.Slice(8), 0);
        BinaryPrimitives.WriteUInt16BigEndian(hdr.Slice(10), 0);
        ms.Write(hdr);
        WriteName(ms, name);
        Span<byte> q = stackalloc byte[4];
        BinaryPrimitives.WriteUInt16BigEndian(q.Slice(0), 12);        // QTYPE PTR
        BinaryPrimitives.WriteUInt16BigEndian(q.Slice(2), 0x8001);    // QCLASS IN + unicast-response (QU) bit
        ms.Write(q);
        return ms.ToArray();
    }

    private static void WriteName(MemoryStream ms, string name)
    {
        foreach (var label in name.Split('.'))
        {
            var b = Encoding.ASCII.GetBytes(label);
            ms.WriteByte((byte)b.Length);
            ms.Write(b, 0, b.Length);
        }
        ms.WriteByte(0);
    }

    private static void ParseResponse(byte[] buf, HashSet<string> ptr,
        Dictionary<string, (string, int)> srv, Dictionary<string, Dictionary<string, string>> txt,
        Dictionary<string, string> a)
    {
        if (buf.Length < 12) return;
        int qd = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(4));
        int an = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(6));
        int ns = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(8));
        int ar = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(10));
        int pos = 12;
        for (int i = 0; i < qd; i++)
        {
            ReadName(buf, ref pos);
            pos += 4; // qtype+qclass
        }
        int total = an + ns + ar;
        for (int i = 0; i < total; i++)
        {
            string name = ReadName(buf, ref pos);
            if (pos + 10 > buf.Length) return;
            int type = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(pos)); pos += 2;
            pos += 2; // class
            pos += 4; // ttl
            int rdlen = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(pos)); pos += 2;
            int rdStart = pos;
            if (rdStart + rdlen > buf.Length) return;

            switch (type)
            {
                case 12: // PTR → instance name
                {
                    int p = rdStart;
                    string inst = ReadName(buf, ref p);
                    if (inst.Contains(Service, StringComparison.OrdinalIgnoreCase)) ptr.Add(inst);
                    break;
                }
                case 33: // SRV → priority(2) weight(2) port(2) target
                {
                    int p = rdStart;
                    p += 4; // priority + weight
                    int port = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(p)); p += 2;
                    string target = ReadName(buf, ref p);
                    srv[name] = (target, port);
                    break;
                }
                case 16: // TXT → length-prefixed key=val strings
                {
                    var kv = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    int p = rdStart;
                    while (p < rdStart + rdlen)
                    {
                        int slen = buf[p++];
                        if (slen == 0 || p + slen > rdStart + rdlen) break;
                        string entry = Encoding.UTF8.GetString(buf, p, slen); p += slen;
                        int eq = entry.IndexOf('=');
                        if (eq > 0) kv[entry.Substring(0, eq)] = entry.Substring(eq + 1);
                    }
                    txt[name] = kv;
                    break;
                }
                case 1: // A → IPv4
                {
                    if (rdlen == 4)
                        a[name] = $"{buf[rdStart]}.{buf[rdStart + 1]}.{buf[rdStart + 2]}.{buf[rdStart + 3]}";
                    break;
                }
            }
            pos = rdStart + rdlen;
        }
    }

    // DNS name reader with 0xC0 compression-pointer support.
    private static string ReadName(byte[] buf, ref int pos)
    {
        var sb = new StringBuilder();
        int p = pos;
        bool jumped = false;
        int guard = 0;
        while (true)
        {
            if (p >= buf.Length || guard++ > 128) break;
            int len = buf[p];
            if (len == 0) { p++; break; }
            if ((len & 0xC0) == 0xC0)
            {
                int ptrTo = ((len & 0x3F) << 8) | buf[p + 1];
                if (!jumped) { pos = p + 2; jumped = true; }
                p = ptrTo;
                continue;
            }
            p++;
            if (p + len > buf.Length) break;
            if (sb.Length > 0) sb.Append('.');
            sb.Append(Encoding.ASCII.GetString(buf, p, len));
            p += len;
        }
        if (!jumped) pos = p;
        return sb.ToString();
    }
}
