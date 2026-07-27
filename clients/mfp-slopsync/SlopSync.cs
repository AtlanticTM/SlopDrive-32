#:name SlopSync
#:version 0.3.0
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
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using MultiFunPlayer.Common;
using MultiFunPlayer.Plugin;
using MultiFunPlayer.Script;
using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
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
//
// v0.3.0 — SlopSync v1.0 wire. Three things changed shape:
//   * §8.4 / RFC-015 READINESS GATE. The hub emits no data-plane frame and
//     NACKs every INTENT `NOT_READY` until the session declares which catalog
//     it decodes against. HELLO now carries a cached `catalog_etag` (the
//     zero-frame reconnect) and, when that misses, the client fetches the
//     catalog over BLOB_REQ/BLOB_CHUNK (0x1A/0x1B — CATALOG_REQ/CHUNK are
//     retired), verifies the SHA-256 locally, and sends CATALOG_READY (0x19).
//   * RFC-006(b) FIELD ROLES. This client used to be publish-only and knew
//     nothing about the machine. It now SUBSCRIBES, and finds the machine's
//     stroke window and kinematic ceilings by their registry `field_roles`
//     rather than by channel number — the catalog is both the map and the
//     decoder ring. Search this file for 0x0081: it is not here.
//   * OPERATOR INTENTS. Home (0x0103 op 1) and a stroke-window editor written
//     through whatever settings channel + keys the ROLE says to use. INTENT
//     frames set header.seq = intent_id so RFC-001's NACK `intent_seq` names
//     the same number the ECHO does.
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
    private StreamMode _mode = StreamMode.Samples;

    [JsonProperty] public string Address { get => _address; set => SetAndNotify(ref _address, value); }
    [JsonProperty] public int Port { get => _port; set => SetAndNotify(ref _port, value); }
    [JsonProperty] public int UpdateRateHz { get => _updateRateHz; set => SetAndNotify(ref _updateRateHz, value); }
    [JsonProperty] public string SourceAxis { get => _sourceAxis; set => SetAndNotify(ref _sourceAxis, value); }
    [JsonProperty] public string PairingPin { get => _pairingPin; set => SetAndNotify(ref _pairingPin, value); }

    // Streaming mode (§0x0084 samples vs §0x0085 timed segments). Persisted by
    // name so reordering the enum can never silently remap a saved value.
    [JsonProperty][JsonConverter(typeof(StringEnumConverter))]
    public StreamMode Mode
    {
        get => _mode;
        set { if (SetAndNotify(ref _mode, value)) NotifyOfPropertyChange(nameof(IsSegmentsMode)); }
    }

    // Bound as the mode ComboBox's ItemsSource (Enum.GetValues gives the members).
    public Array Modes => Enum.GetValues(typeof(StreamMode));

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
    private long _segmentsSent;
    private string _divergenceWarning;
    private DiscoveredDevice _selectedDevice;
    private bool _isDiscovering;

    public ConnectionStatus Status
    {
        get => _status;
        set
        {
            if (!SetAndNotify(ref _status, value)) return;
            NotifyOfPropertyChange(nameof(IsEditable));
            NotifyOfPropertyChange(nameof(IsConnected));
            NotifyOfPropertyChange(nameof(IsWindowEditable));
        }
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
    public long SegmentsSent { get => _segmentsSent; set => SetAndNotify(ref _segmentsSent, value); }
    public string DivergenceWarning { get => _divergenceWarning; set => SetAndNotify(ref _divergenceWarning, value); }

    // ---- Device readback (GROUND TRUTH — written only from STATE / ECHO) ----
    private string _catalogInfo;
    private double _windowMinMm = double.NaN, _windowMaxMm = double.NaN;
    private double _inputSpeed = double.NaN, _inputAccel = double.NaN, _inputJerk = double.NaN;
    private double _windowMinEdit, _windowMaxEdit;
    private string _windowStatus, _homeStatus;

    public string CatalogInfo { get => _catalogInfo; set => SetAndNotify(ref _catalogInfo, value); }

    /// <summary>Device-reported stroke window, mm. NaN until a STATE says otherwise.</summary>
    public double WindowMinMm
    {
        get => _windowMinMm;
        set { if (SetAndNotify(ref _windowMinMm, value)) NotifyOfPropertyChange(nameof(WindowText)); }
    }
    public double WindowMaxMm
    {
        get => _windowMaxMm;
        set { if (SetAndNotify(ref _windowMaxMm, value)) NotifyOfPropertyChange(nameof(WindowText)); }
    }

    /// <summary>The operator's DRAFT values — never displayed as device truth.</summary>
    public double WindowMinEdit
    {
        get => _windowMinEdit;
        set { if (SetAndNotify(ref _windowMinEdit, value)) _windowDirty = true; }
    }
    public double WindowMaxEdit
    {
        get => _windowMaxEdit;
        set { if (SetAndNotify(ref _windowMaxEdit, value)) _windowDirty = true; }
    }

    public string WindowStatus { get => _windowStatus; set => SetAndNotify(ref _windowStatus, value); }
    public string HomeStatus { get => _homeStatus; set => SetAndNotify(ref _homeStatus, value); }

    /// <summary>The device's window as one line — this is the value the machine
    /// has, not the one in the boxes.</summary>
    public string WindowText => double.IsNaN(_windowMinMm) || double.IsNaN(_windowMaxMm)
        ? "—"
        : $"{_windowMinMm:F1} – {_windowMaxMm:F1} mm";

    // ---- Machine limits — READ-ONLY DISPLAY, and that is the whole contract --
    // RFC-008: the plugin ships the sender's intent AS AUTHORED. It does not
    // map, clamp, scale or pre-adapt anything to these numbers, and there is
    // deliberately no code path that could — MFP plays a funscript, it cannot
    // make authored content more machine-compatible, and the MACHINE plays back
    // whatever it is fed as well as it possibly can. These exist so an operator
    // can SEE what the machine's ceilings are. Nothing else consumes them.
    public double InputSpeed
    {
        get => _inputSpeed;
        set { if (SetAndNotify(ref _inputSpeed, value)) NotifyOfPropertyChange(nameof(InputSpeedText)); }
    }
    public double InputAccel
    {
        get => _inputAccel;
        set { if (SetAndNotify(ref _inputAccel, value)) NotifyOfPropertyChange(nameof(InputAccelText)); }
    }
    public double InputJerk
    {
        get => _inputJerk;
        set { if (SetAndNotify(ref _inputJerk, value)) NotifyOfPropertyChange(nameof(InputJerkText)); }
    }

    public string InputSpeedText => Fmt(_inputSpeed, "mm/s");
    public string InputAccelText => Fmt(_inputAccel, "mm/s²");
    public string InputJerkText => Fmt(_inputJerk, "mm/s³");

    private static string Fmt(double v, string unit) =>
        double.IsNaN(v) ? "not advertised" : $"{v:N0} {unit}";

    /// <summary>True once this hub advertised a WRITABLE window through the
    /// window.min / window.max roles. False on a hub that does not — the card
    /// greys rather than pretending it can write something it cannot find.</summary>
    public bool HasWindowControl =>
        (_roleWindowMin != null && _roleWindowMin.Writable) ||
        (_roleWindowMax != null && _roleWindowMax.Writable);

    public bool HasLimitsReadback =>
        _roleInputSpeed != null || _roleInputAccel != null || _roleInputJerk != null;

    public bool IsWindowEditable => IsConnected && HasWindowControl && !_windowPending;

    public bool IsConnected => Status == ConnectionStatus.Connected;

    // True only while fully disconnected — the view binds edit boxes' IsEnabled here.
    public bool IsEditable => Status == ConnectionStatus.Disconnected;

    /// <summary>Subscribed rate for the live motion STATE feed (§10.2 wish).</summary>
    private const double MotionStateRateHz = 20.0;

    // The view shows the Segments-only rows (counter + divergence line) off this.
    public bool IsSegmentsMode => Mode == StreamMode.Segments;

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

    // ---- Segments-mode engine state -----------------------------------------
    // The CONNECTION TASK owns the cursor and the emitter (SegTickAsync, driven
    // by SegmentLoopAsync's tick). MFP's message thread only ever RAISES flags
    // here — it never walks keyframes, never emits, never touches the socket.
    // That split is the v0.2.1 fix: the old design ran the emitter inside
    // HandleMessage(MediaPositionChangedMessage), i.e. at whatever rate the
    // active media source happens to report position, which is nowhere near the
    // script's action rate (see the SEGMENTS MODE header for the measurement).
    // Everything the MFP thread writes is volatile; everything the tick owns
    // (_segEmittedSpan, the position/transform snapshots, the token bucket) is
    // touched from exactly one task and needs no lock.
    private volatile bool _segActive;                       // gates all HandleMessage work
    private volatile KeyframeCollection _segKeyframes;      // ref-swapped on script change (immutable contents)
    private volatile bool _segPlaying;                      // last MediaPlayingChangedMessage (tick prefers the property)
    private volatile bool _segNeedsResync;                  // raised by MFP thread, consumed by the tick
    private volatile KeyframeCollection _segPendingKeyframes; // ScriptChangedMessage's copy; adopted at the next re-anchor
    private DeviceAxis _segAxis;
    private int _segEmittedSpan = -1;                       // last span handed to the wire / skipped; -1 = re-anchor
    private double _segLastAxisPos;                         // discontinuity detector: axis script position last tick
    private bool _segHaveAxisPos;
    private double _segFrozenMs;                            // ms the media clock has not moved (the liveness gate)

    // ---- Emitter diagnostics ------------------------------------------------
    // A silent early return in the emitter is indistinguishable from a broken
    // machine — 0.2.1 shipped with five of them and cost a whole debug cycle.
    // Every non-emitting path now names itself here, abnormal ones warn once,
    // and the 1 Hz heartbeat puts the whole decision state on one line.
    private string _segStop = "starting";                   // why the last tick emitted nothing (null = it emitted)
    private string _segStopWarned;                          // last abnormal reason already warned about
    private double _segBeatAtMs;                        // loop-clock ms of the last heartbeat
    private long _segBeatSegs;                              // localSegs at the last heartbeat
    private double _segBeatPos;                             // last sampled axis position (for the beat)
    private int _segBeatKfCount;
    private string _segStatusBase;                          // "Segments L0 @ 30 Hz …" — the beat is appended to it

    // Per-tick snapshot of the axis transform stages we replicate. Read once per
    // tick instead of per keyframe: TransformValue and the end-velocity slope
    // must agree, and a value that changes halfway through a tick would emit a
    // segment whose target and end velocity came from different transforms.
    private double _segScale = 1.0;
    private bool _segInvert;
    private InterpolationType _segInterp = InterpolationType.Linear;

    // Client-side mirror of the hub's §10.5 token bucket for 0x0085 (sustained
    // rate = granted rate, depth = granted rate). Shaping here means a
    // pathologically dense script section is thinned by US, deliberately and
    // counted, instead of arriving as a burst the hub answers with RATE_LIMITED
    // NACKs and silent drops.
    private double _segTokens;
    private double _segTokenRate;
    private double _segTokenDepth;
    private long _segThrottled;
    private double _segThrottleLogAgeMs;   // rate-limits the throttle warning itself
    private double _segCacheAgeMs;         // ms since the last transform-cache refresh

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

    // ---- The credential (fw 2.1.59+ enforces authorization) ------------------
    // Before 2.1.59 the hub granted `control` to anything that connected, so this
    // plugin never needed a credential and the PIN box fed HELLO for a future it
    // could not yet reach. Now a tokenless HELLO is granted `watch`: telemetry
    // still flows and the e-stop still works (safety ops are role-exempt), but
    // motion-input/motion-segment publishing is refused — which for this plugin
    // means "connects fine, plays nothing".
    //
    // The ladder mirrors the WebUI's (webui/src/core/slopsync/credentials.js):
    //   1. PIN box holding 32 hex chars -> treat it as a literal paired token.
    //      This is the forward-compatible rung: once the pairing ceremony ships,
    //      paste the issued token and it keeps working with /uitoken disabled.
    //   2. GET http://<host>/uitoken -> single-use control-tier mint. No CORS
    //      involved; we are not a browser.
    //   3. null -> connect as a viewer and say so plainly in the log.
    //
    // NOT cached between sessions ON PURPOSE: a mint is single-use with a 60 s
    // TTL, so a stored one is a stale one. Called per connect.
    private static readonly HttpClient _http = new HttpClient { Timeout = TimeSpan.FromSeconds(3) };

    private async Task<byte[]> AcquireTokenAsync(string host, CancellationToken ct)
    {
        var pin = PairingPin ?? "";
        if (pin.Length == SlopWire.TokenBytes * 2 && IsHex(pin))
        {
            Logger.Info("SlopSync credential: paired token from the PIN field");
            return FromHex(pin);
        }

        // Rate-limited to one mint per 250 ms device-wide; a couple of tries is
        // plenty for a single plugin connecting once.
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                using var res = await _http.GetAsync($"http://{host}/uitoken", ct).ConfigureAwait(false);
                if (res.StatusCode == HttpStatusCode.TooManyRequests)
                {
                    await Task.Delay(350, ct).ConfigureAwait(false);
                    continue;
                }
                if (!res.IsSuccessStatusCode)
                {
                    // 403 = the operator disabled /uitoken (the lockdown posture).
                    // That is a configuration choice, not a fault: say what to do.
                    Logger.Warn("SlopSync /uitoken refused ({0}) — pair this client and paste its token in the PIN field", res.StatusCode);
                    return null;
                }
                var body = await res.Content.ReadAsStringAsync().ConfigureAwait(false);
                var j = JsonConvert.DeserializeObject<Newtonsoft.Json.Linq.JObject>(body);
                var tok = (string)j?["token"];
                if (j?["ok"]?.ToObject<bool>() == true && tok != null && tok.Length == SlopWire.TokenBytes * 2 && IsHex(tok))
                {
                    Logger.Info("SlopSync credential: /uitoken mint (control tier)");
                    return FromHex(tok);
                }
                return null;
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception e)
            {
                Logger.Debug("SlopSync /uitoken attempt {0} failed: {1}", attempt, e.Message);
                await Task.Delay(200, ct).ConfigureAwait(false);
            }
        }
        return null;
    }

    private static bool IsHex(string s)
    {
        foreach (var c in s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    }

    private static byte[] FromHex(string s)
    {
        var b = new byte[s.Length / 2];
        for (var i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    // Property changes fired from the background task must be raised on the UI
    // thread for WPF; ViewPlugin.cs does it bare, but marshalling keeps binding
    // exceptions off the socket loop. Execute.OnUIThread is a no-op if already
    // on the dispatcher, so it is cheap.
    private void Ui(Action a) => Execute.OnUIThread(a);

    protected override void OnInitialize()
    {
        // Make connect/disconnect bindable from MFP's shortcut system, mirroring
        // a native output target's "<Identifier>::Connection::{Toggle,Connect,
        // Disconnect}" convention. OnConnectClick() is itself a toggle; Connect/
        // Disconnect are idempotent guards on the task handle. PluginBase tracks
        // these and auto-unregisters them on dispose. Marshalled to the UI thread
        // because they mutate the same _task/_cancellationSource the view does.
        RegisterAction("SlopSync::Connection::Toggle", () => Ui(OnConnectClick));
        RegisterAction("SlopSync::Connection::Connect", () => Ui(() => { if (_task == null) OnConnectClick(); }));
        RegisterAction("SlopSync::Connection::Disconnect", () => Ui(() => { if (_task != null) OnConnectClick(); }));
    }

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
        // Snapshot the mode for the whole session — the view disables the mode
        // ComboBox while connected, so it cannot change under us, but reading it
        // once keeps the branch decision stable regardless.
        var mode = Mode;
        double wishHz = Math.Clamp(UpdateRateHz, 10, 250);
        // WAS: Encoding.UTF8.GetBytes(PairingPin) — which could never validate.
        // The hub's token field is exactly 16 BYTES; a PIN typed as text is
        // neither that length nor that encoding, so it was silently ignored back
        // when everything was granted control anyway. Under enforcement that
        // would present as "connects, plays nothing". See AcquireTokenAsync.
        byte[] token16 = await AcquireTokenAsync(Address, token).ConfigureAwait(false);
        if (token16 == null)
            Logger.Warn("SlopSync connecting WITHOUT a credential — viewer tier: telemetry and e-stop work, playback will not");

        // Samples mode wishes 0x0084 only (unchanged). Segments mode ALSO wishes
        // 0x0084 (the dense-sample fallback path stays granted) PLUS 0x0085.
        //
        // The wish becomes the hub's §10.5 SUSTAINED token-bucket ceiling
        // (granted = min(wish, 50 Hz cap); bucket depth == granted rate). Segments
        // average only a handful/sec, but a dense/fast funscript section (rapid
        // strokes / vibration) bursts far higher — and the OLD 10 Hz wish throttled
        // exactly those sections to one segment per 100 ms (the ~99 ms lock the
        // field saw): once the 10-token burst budget drained, the bucket refilled a
        // single token every 100 ms. Segments are emitted the instant the emitter
        // tick sees an action come due (SegTickAsync, 100 Hz), so the wish must
        // clear the worst-case burst, not the average. 30 Hz = 30 Hz sustained +
        // a full-second 30-token burst budget — headroom over the ~25 Hz worst
        // case, still under the channel's 50 Hz cap. The plugin now shapes to
        // this same grant client-side so the hub never has to NACK us.

        // Cached catalog for THIS host: presenting a matching etag in HELLO
        // makes the session ready at WELCOME with no transfer at all (RFC-015).
        // Re-verified before it is claimed — declaring an etag we cannot back
        // up with the bytes we hold would be a lie the hub cannot detect.
        _catalogCacheKey = $"{Address}:{Port}";
        var cached = LoadCachedCatalog(_catalogCacheKey);

        // §6.2 subscription wishes ride along in HELLO — one round trip, and
        // RFC-006's point: a motion producer that subscribes to nothing is
        // flying blind. The kinematic-limits channel is NOT in this list; it
        // has no portable number and is only knowable once the catalog is
        // decoded, so it is a mid-session SUBSCRIBE below.
        var subWishes = new (ushort ch, double rate, byte prio)[]
        {
            (SlopWire.ChSafety, 0.0, SlopWire.PriorityCritical),
            (SlopWire.ChMotion, MotionStateRateHz, SlopWire.PriorityElevated),
        };

        WelcomeInfo welcome;
        if (mode == StreamMode.Segments)
        {
            welcome = await client.HelloAsync("mfp", "MultiFunPlayer SlopSync",
                new (ushort ch, double rate)[] { (SlopWire.ChMotionInput, wishHz), (SlopWire.ChMotionSegment, SegmentWishHz) },
                token16, token, subWishes, cached?.Etag);
        }
        else
        {
            welcome = await client.HelloAsync("mfp", "MultiFunPlayer SlopSync",
                new (ushort ch, double rate)[] { (SlopWire.ChMotionInput, wishHz) },
                token16, token, subWishes, cached?.Etag);
        }
        _cachedForThisSession = cached;

        // ---- §8.4 / RFC-015 READINESS GATE ----------------------------------
        // Nothing below this point works until the hub knows which catalog we
        // decode against: no retained STATE, no STREAM ingress, and every
        // INTENT answered NOT_READY. Two paths — see HubClient's readiness
        // comment for why the cached-etag one is the common case.
        await EstablishReadiness(client, welcome, token);

        double granted = welcome.GrantedPublishRate(SlopWire.ChMotionInput);
        if (double.IsNaN(granted))
            throw new InvalidOperationException("no publish grant for motion-input(0x0084) in WELCOME");

        // Segments mode *requires* the 0x0085 grant — mirror the no-grant error
        // path rather than silently degrading to the sample loop (ground-truth
        // doctrine: the UI says "Segments", so we send segments or we error).
        double segGranted = double.NaN;
        if (mode == StreamMode.Segments)
        {
            segGranted = welcome.GrantedPublishRate(SlopWire.ChMotionSegment);
            if (double.IsNaN(segGranted))
                throw new InvalidOperationException("no publish grant for motion-segment(0x0085) in WELCOME — device may predate the segment channel; use Samples mode");
        }

        double segGrantedSnapshot = segGranted;
        // Segments mode keeps its base status line around: the 1 Hz heartbeat
        // appends the emitter's decision state to it (see SegHeartbeat).
        _segStatusBase = mode == StreamMode.Segments
            ? $"Segments {SourceAxis} @ {segGrantedSnapshot:F0} Hz (fallback {granted:F0} Hz)"
            : null;
        string statusText = _segStatusBase ?? $"Streaming {SourceAxis} @ {granted:F0} Hz";
        Ui(() =>
        {
            SessionId = welcome.SessionId;
            GrantedRate = granted;
            DeviceInfo = _selectedDevice != null && !string.IsNullOrEmpty(_selectedDevice.Fw)
                ? $"{_selectedDevice.InstanceName} · fw {_selectedDevice.Fw}"
                : $"boot 0x{welcome.BootId:X8}";
            Status = ConnectionStatus.Connected;
            StatusText = statusText;
            NotifyOfPropertyChange(nameof(IsEditable));
        });
        Logger.Info("WELCOME: session={0} boot=0x{1:X8} mode={2} granted motion-input @ {3:F1} Hz (wished {4:F1}){5}",
            welcome.SessionId, welcome.BootId, mode, granted, wishHz,
            mode == StreamMode.Segments ? $", motion-segment @ {segGranted:F1} Hz" : "");

        // ---- SUBSCRIBE: the role-located limits channel ----------------------
        // safety(0x0003) + motion(0x0080) were already wished in HELLO. What is
        // left is whatever channel THIS hub happens to carry the kinematic
        // roles on — resolved from the catalog, never hardcoded (RFC-006(b)).
        // A hub that declares none of those roles gets no extra SUBSCRIBE and
        // the panel says "not advertised", which is the honest answer.
        var limitsChannels = RoleChannelsToSubscribe();
        if (limitsChannels.Count > 0)
        {
            await client.SubscribeAsync(
                limitsChannels.ConvertAll(ch => ((ushort)ch, 0.0, SlopWire.PriorityNormal)), token);
            Logger.Info("SUBSCRIBEd to role-located channel(s): {0}",
                string.Join(", ", limitsChannels.ConvertAll(c => $"0x{c:X4}")));
        }
        else
        {
            Logger.Info("this hub advertises none of the kinematic field_roles — limits panel will read 'not advertised'");
        }

        // ---- Receive loop (routes CLOCK replies, NACKs, STATE, ECHO, PING) --
        var recvTask = client.ReceiveLoopAsync(OnNack, OnState, token, OnEcho);
        _client = client;

        // ---- Initial CLOCK sync ---------------------------------------------
        try
        {
            await ResyncClock(client, token);

            // ---- Stream loop (mode-specific) -------------------------------------
            if (mode == StreamMode.Segments)
                await SegmentLoopAsync(client, granted, segGranted, token);
            else
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
            _client = null;
            _intentQueue.Clear();
            Ui(ClearDeviceReadback);
        }

        await recvTask;
    }

    private void OnNack(HubClient.NackInfo n)
    {
        Ui(() =>
        {
            NackCount++;
            if (n.Code == SlopWire.NackRateLimited) RateLimitedCount++;
            // RFC-001 correlation: our INTENT frames carry header.seq ==
            // intent_id, so this names the exact intent that was refused.
            if (n.IntentSeq.HasValue && n.IntentSeq.Value == _windowIntentId && _windowPending)
            {
                _windowPending = false;
                WindowStatus = $"REFUSED: {n.Name}";
                NotifyOfPropertyChange(nameof(IsWindowEditable));
            }
            if (n.IntentSeq.HasValue && n.IntentSeq.Value == _homeIntentId && _homePending)
            {
                _homePending = false;
                HomeStatus = $"REFUSED: {n.Name}";
            }
        });
        Logger.Warn("NACK {0} channel=0x{1:X4} intent_seq={2}{3}",
            n.Name, n.Channel, n.IntentSeq?.ToString() ?? "-",
            string.IsNullOrEmpty(n.Detail) ? "" : $" detail={n.Detail}");
    }

    private void OnState(ushort channel, byte[] payload)
    {
        Ui(() => StatesReceived++);
        // Ground truth in, optimism never out: every displayed device value
        // below comes from THIS packed snapshot, decoded against the layout the
        // hub itself published.
        AdoptRoleReadback(channel, payload);
    }

    private void OnEcho(HubClient.EchoInfo e)
    {
        // §9.3 + ground-truth doctrine: the ECHO carries APPLIED (post-clamp)
        // values. A control that showed what we ASKED for would be a UI that
        // lies about machine state, which on this product is a safety defect.
        Ui(() =>
        {
            if (_windowPending && e.IntentId == _windowIntentId)
            {
                _windowPending = false;
                string applied = null;
                if (_roleWindowMin?.SettingKey is int kMin && e.TryGetApplied(kMin, out var vMin))
                { WindowMinMm = vMin; applied = $"min {vMin:F1}"; }
                if (_roleWindowMax?.SettingKey is int kMax && e.TryGetApplied(kMax, out var vMax))
                { WindowMaxMm = vMax; applied = (applied == null ? "" : applied + " / ") + $"max {vMax:F1}"; }
                // Re-seed the drafts from what actually applied, so the boxes
                // can never sit showing a value the machine refused to take.
                WindowMinEdit = WindowMinMm;
                WindowMaxEdit = WindowMaxMm;
                WindowStatus = applied == null ? "applied" : $"applied: {applied} mm";
                NotifyOfPropertyChange(nameof(IsWindowEditable));
            }
            if (_homePending && e.IntentId == _homeIntentId)
            {
                _homePending = false;
                HomeStatus = "homing accepted";
            }
        });
        Logger.Info("ECHO channel=0x{0:X4} intent_id={1} cfg_gen={2}", e.Channel, e.IntentId, e.CfgGen);
    }

    // =========================================================================
    // §8.4 / RFC-015 readiness + RFC-006(b) role resolution
    // =========================================================================

    /// <summary>One cached catalog for one host. In MEMORY only, deliberately:
    /// MFP's single persistence hook is the plugin's settings JSON, and stuffing
    /// 20 kB of hex catalog into a user's config file to save one 10 kB transfer
    /// per MFP launch is a bad trade. This still covers the case that matters —
    /// every reconnect, backoff retry and mode switch inside one MFP session
    /// takes the zero-frame fast path.</summary>
    private sealed class CachedCatalog
    {
        public byte[] Etag;
        public byte[] Bytes;
        public SlopCatalog Catalog;
    }

    private static readonly Dictionary<string, CachedCatalog> _catalogCache = new();
    private static readonly object _catalogCacheLock = new();
    private string _catalogCacheKey;
    private CachedCatalog _cachedForThisSession;

    // The adopted decoder ring + the resolved role locators for this session.
    private SlopCatalog _catalog;
    private SlopCatalog.RoleLocator _roleWindowMin, _roleWindowMax;
    private SlopCatalog.RoleLocator _roleInputSpeed, _roleInputAccel, _roleInputJerk;

    private static CachedCatalog LoadCachedCatalog(string key)
    {
        lock (_catalogCacheLock)
        {
            if (!_catalogCache.TryGetValue(key, out var c)) return null;
            // Re-verify before claiming: the etag we present in HELLO must
            // actually be the hash of the bytes we hold.
            if (SlopCatalog.BytesEqual(SlopCatalog.Etag(c.Bytes), c.Etag)) return c;
            _catalogCache.Remove(key);
            return null;
        }
    }

    private static void StoreCachedCatalog(string key, CachedCatalog c)
    {
        lock (_catalogCacheLock) { _catalogCache[key] = c; }
    }

    private async Task EstablishReadiness(HubClient client, WelcomeInfo welcome, CancellationToken token)
    {
        _catalog = null;
        _roleWindowMin = _roleWindowMax = null;
        _roleInputSpeed = _roleInputAccel = _roleInputJerk = null;

        byte[] hubEtag = welcome.CatalogEtag;
        if (hubEtag == null || hubEtag.Length != SlopWire.EtagBytes)
            throw new InvalidOperationException(
                $"WELCOME carried no usable {SlopWire.EtagBytes}-byte catalog_etag — cannot declare readiness");

        // ---- Fast path: the hub agreed with the etag we put in HELLO --------
        // It already set our ready bit when it read that etag, so retained STATE
        // is on its way. NO CATALOG_READY frame — that is the whole point.
        if (_cachedForThisSession != null && SlopCatalog.BytesEqual(_cachedForThisSession.Etag, hubEtag))
        {
            _readyPending = false;   // nothing was sent, so there is nothing to re-declare
            AdoptCatalog(_cachedForThisSession.Catalog, hubEtag, cachedPath: true);
            return;
        }

        // ---- Fetch path: BLOB_REQ -> chunks -> verify LOCALLY -> READY ------
        Ui(() => StatusText = "Fetching catalog");
        var bytes = await client.FetchCatalogAsync(token);
        if (bytes == null)
            throw new InvalidOperationException(
                "catalog transfer failed — cannot declare readiness, and an un-ready session is GOODBYE'd (READY_TIMEOUT)");

        var digest = SlopCatalog.Etag(bytes);
        bool verified = SlopCatalog.BytesEqual(digest, hubEtag);
        if (!verified)
        {
            // Honest-degraded: declare the digest of what we ACTUALLY hold, so
            // the hub can flag the mismatch instead of being misled by a claim
            // we cannot back up.
            Logger.Warn("catalog reassembled to etag {0} but WELCOME advertised {1} — declaring what we hold",
                SlopCatalog.Hex(digest), SlopCatalog.Hex(hubEtag));
        }

        var cat = SlopCatalog.Decode(bytes);
        if (cat == null)
            throw new InvalidOperationException("catalog bytes did not decode — no decoder ring, refusing to stream");

        await client.SendCatalogReadyAsync(digest, token);
        Logger.Info("CATALOG_READY sent (etag {0}, {1} B, {2} channels{3})",
            SlopCatalog.Hex(digest), bytes.Length, cat.Entries.Count, verified ? "" : ", UNVERIFIED");

        // Arm the re-declaration cadence (§8.4). Over a WebSocket the frame
        // cannot actually be lost, so this is belt-and-braces for the day this
        // client speaks a lossy transport — and it is free: declaring readiness
        // is an IDEMPOTENT flag-set on the hub, so a redundant 16-byte frame
        // costs nothing but 16 bytes. It stops at the first STATE (proof the
        // data plane opened) and is bounded by the hub's own
        // catalog_ready_timeout_ms, past which the hub has already GOODBYE'd us
        // with READY_TIMEOUT and re-declaring would be pure noise.
        _readyEtag = digest;
        _readyPending = true;
        _readyAttempts = 1;
        _readyLastSendMs = 0;

        StoreCachedCatalog(_catalogCacheKey, new CachedCatalog { Etag = digest, Bytes = bytes, Catalog = cat });
        AdoptCatalog(cat, digest, cachedPath: false);
    }

    /// <summary>Adopt a catalog as this session's decoder ring and resolve the
    /// field roles this client cares about. Everything downstream reads the
    /// locators; nothing reads a channel number.</summary>
    private void AdoptCatalog(SlopCatalog cat, byte[] etag, bool cachedPath)
    {
        _catalog = cat;
        _roleWindowMin = cat.LocateRole(SlopWire.RoleWindowMin);
        _roleWindowMax = cat.LocateRole(SlopWire.RoleWindowMax);
        _roleInputSpeed = cat.LocateRole(SlopWire.RoleLimitInputSpeed);
        _roleInputAccel = cat.LocateRole(SlopWire.RoleLimitInputAccel);
        _roleInputJerk = cat.LocateRole(SlopWire.RoleLimitInputJerk);

        Logger.Info("catalog adopted ({0}, etag {1}): window.min={2} window.max={3} " +
                    "limit.input.speed={4} .accel={5} .jerk={6}",
            cachedPath ? "cached, zero-frame path" : "fetched + verified", SlopCatalog.Hex(etag),
            Describe(_roleWindowMin), Describe(_roleWindowMax),
            Describe(_roleInputSpeed), Describe(_roleInputAccel), Describe(_roleInputJerk));

        Ui(() =>
        {
            CatalogInfo = $"{cat.Entries.Count} ch · {cat.RoleCount} roles · {SlopCatalog.Hex(etag)}"
                        + (cachedPath ? " (cached)" : "");
            NotifyOfPropertyChange(nameof(HasWindowControl));
            NotifyOfPropertyChange(nameof(HasLimitsReadback));
            NotifyOfPropertyChange(nameof(IsWindowEditable));
        });

        static string Describe(SlopCatalog.RoleLocator r) =>
            r == null ? "-" : $"0x{r.ChannelId:X4}.{r.Field.Name}@{r.Field.Offset}{(r.Writable ? "(w)" : "(ro)")}";
    }

    /// <summary>The distinct STATE channels our resolved roles live on. Usually
    /// one; the code does not assume that, because nothing in the spec says a
    /// hub must keep window and limits on the same channel.</summary>
    private List<int> RoleChannelsToSubscribe()
    {
        var set = new List<int>();
        void Add(SlopCatalog.RoleLocator r)
        {
            if (r == null) return;
            if (r.ChannelId == SlopWire.ChSafety || r.ChannelId == SlopWire.ChMotion) return; // already wished
            if (!set.Contains(r.ChannelId)) set.Add(r.ChannelId);
        }
        Add(_roleWindowMin); Add(_roleWindowMax);
        Add(_roleInputSpeed); Add(_roleInputAccel); Add(_roleInputJerk);
        return set;
    }

    /// <summary>Decode a STATE snapshot against the adopted layout and take any
    /// role value it carries. Values that are NOT on this channel are untouched.</summary>
    private void AdoptRoleReadback(ushort channel, byte[] payload)
    {
        if (_catalog == null || payload == null || payload.Length == 0) return;

        double? wMin = Read(_roleWindowMin), wMax = Read(_roleWindowMax);
        double? spd = Read(_roleInputSpeed), acc = Read(_roleInputAccel), jrk = Read(_roleInputJerk);
        if (wMin == null && wMax == null && spd == null && acc == null && jrk == null) return;

        Ui(() =>
        {
            if (wMin.HasValue) { WindowMinMm = wMin.Value; if (!_windowPending && !_windowDirty) WindowMinEdit = wMin.Value; }
            if (wMax.HasValue) { WindowMaxMm = wMax.Value; if (!_windowPending && !_windowDirty) WindowMaxEdit = wMax.Value; }
            if (spd.HasValue) InputSpeed = spd.Value;
            if (acc.HasValue) InputAccel = acc.Value;
            if (jrk.HasValue) InputJerk = jrk.Value;
            NotifyOfPropertyChange(nameof(HasLimitsReadback));
        });

        double? Read(SlopCatalog.RoleLocator r)
        {
            if (r == null || r.ChannelId != channel) return null;
            double v = SlopCatalog.ReadField(payload, r.Field);
            return double.IsNaN(v) ? (double?)null : v;
        }
    }

    private void ClearDeviceReadback()
    {
        CatalogInfo = null;
        WindowMinMm = WindowMaxMm = double.NaN;
        InputSpeed = InputAccel = InputJerk = double.NaN;
        WindowStatus = null;
        HomeStatus = null;
        _windowPending = _homePending = false;
        _windowDirty = false;
        NotifyOfPropertyChange(nameof(HasWindowControl));
        NotifyOfPropertyChange(nameof(HasLimitsReadback));
        NotifyOfPropertyChange(nameof(IsWindowEditable));
    }

    // =========================================================================
    // OPERATOR CONTROLS — Home + the mini stroke-window editor.
    //
    // Both go through an INTENT QUEUE drained by the connection task, for the
    // same reason the segment emitter lives there: the connection task owns the
    // socket. The UI thread only ever enqueues.
    //
    // GROUND-TRUTH DOCTRINE, applied literally: the displayed device values
    // (WindowMinMm/WindowMaxMm, InputSpeed/Accel/Jerk) are written ONLY from a
    // decoded STATE snapshot or an ECHO's post-clamp `applied` map. The edit
    // boxes are a separate DRAFT (WindowMinEdit/WindowMaxEdit) that is re-seeded
    // from device truth whenever the operator is not mid-edit and after every
    // echo. Nothing optimistic ever reaches the "device" column.
    // =========================================================================
    private readonly System.Collections.Concurrent.ConcurrentQueue<PendingIntent> _intentQueue = new();
    private HubClient _client;
    private long _intentSeqCounter;
    private long _windowIntentId = -1, _homeIntentId = -1;
    private bool _windowPending, _homePending;
    private bool _windowDirty;   // operator has touched a box since the last echo/state seed

    private readonly struct PendingIntent
    {
        public readonly ushort Channel;
        public readonly long IntentId;
        public readonly (int key, byte[] encoded)[] Value;
        public PendingIntent(ushort ch, long id, (int, byte[])[] v) { Channel = ch; IntentId = id; Value = v; }
    }

    /// <summary>Drain queued intents. Called from both stream loops' ticks, i.e.
    /// on the connection task, i.e. the only place allowed to write the socket.</summary>
    private async Task DrainIntentsAsync(HubClient client, CancellationToken token)
    {
        while (_intentQueue.TryDequeue(out var p))
        {
            try { await client.SendIntentAsync(p.Channel, p.IntentId, p.Value, token); }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex) { Logger.Warn(ex, "INTENT send failed (channel 0x{0:X4})", p.Channel); }
        }
    }

    // ---- CATALOG_READY re-declaration cadence (§8.4) ------------------------
    private byte[] _readyEtag;
    private bool _readyPending;
    private int _readyAttempts;
    private double _readyLastSendMs;
    private const int ReadyMaxAttempts = 8;   // × 500 ms stays well inside catalog_ready_timeout_ms

    /// <summary>Re-declare readiness until the first STATE proves the data plane
    /// opened. Idempotent on the hub; called from both stream loops' ticks.</summary>
    private async Task PumpCatalogReadyAsync(HubClient client, double nowMs, CancellationToken token)
    {
        if (!_readyPending) return;
        if (StatesReceived > 0) { _readyPending = false; return; }   // the gate is demonstrably open
        if (_readyAttempts >= ReadyMaxAttempts) { _readyPending = false; return; }
        if (nowMs - _readyLastSendMs < SlopWire.CatalogChunkGapTimeoutMs) return;
        _readyLastSendMs = nowMs;
        _readyAttempts++;
        try { await client.SendCatalogReadyAsync(_readyEtag, token); }
        catch (OperationCanceledException) { throw; }
        catch (Exception ex) { Logger.Debug(ex, "CATALOG_READY re-declare failed"); }
    }

    // ---- Home (INTENT 0x0103 op 1) ------------------------------------------
    public void OnHomeClick()
    {
        if (_client == null) { HomeStatus = "not connected"; return; }
        _homeIntentId = System.Threading.Interlocked.Increment(ref _intentSeqCounter);
        _homePending = true;
        HomeStatus = "homing requested…";
        _intentQueue.Enqueue(new PendingIntent(SlopWire.ChHome, _homeIntentId,
            new (int, byte[])[] { (1, SlopWire.CborUInt(SlopWire.HomeOpHome)) }));
        Logger.Info("HOME intent queued (intent_id={0})", _homeIntentId);
    }

    // ---- Stroke window (INTENT on the role's paired settingChannel) ---------
    // The channel and the keys are BOTH taken from the role locators, so this
    // writes 0x0101 keys 1/2 on this device purely because that is what its
    // catalog says window.min / window.max are written with.
    public void OnApplyWindowClick()
    {
        if (_client == null) { WindowStatus = "not connected"; return; }
        if (!HasWindowControl) { WindowStatus = "hub does not advertise a writable stroke window"; return; }

        var chan = _roleWindowMin?.SettingChannel ?? _roleWindowMax?.SettingChannel;
        if (chan == null) { WindowStatus = "no paired settings channel"; return; }

        var fields = new List<(int, byte[])>();
        if (_roleWindowMin?.SettingKey is int kMin) fields.Add((kMin, SlopWire.CborF32(WindowMinEdit)));
        if (_roleWindowMax?.SettingKey is int kMax) fields.Add((kMax, SlopWire.CborF32(WindowMaxEdit)));
        if (fields.Count == 0) { WindowStatus = "nothing writable"; return; }
        fields.Sort((a, b) => a.Item1.CompareTo(b.Item1));   // §5.3: map keys ascending

        _windowIntentId = System.Threading.Interlocked.Increment(ref _intentSeqCounter);
        _windowPending = true;
        _windowDirty = false;
        WindowStatus = "pending…";
        NotifyOfPropertyChange(nameof(IsWindowEditable));
        _intentQueue.Enqueue(new PendingIntent(chan.Value, _windowIntentId, fields.ToArray()));
        Logger.Info("window INTENT queued: channel=0x{0:X4} intent_id={1} min={2:F1} max={3:F1}",
            chan.Value, _windowIntentId, WindowMinEdit, WindowMaxEdit);
    }

    /// <summary>Discard the draft and re-seed both boxes from device truth.</summary>
    public void OnRevertWindowClick()
    {
        _windowDirty = false;
        WindowMinEdit = WindowMinMm;
        WindowMaxEdit = WindowMaxMm;
        WindowStatus = null;
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

            // Operator intents (Home / stroke window) queued by the UI thread.
            // Drained here because the connection task owns the socket.
            await DrainIntentsAsync(client, token);
            await PumpCatalogReadyAsync(client, nowMs, token);

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
    // SEGMENTS MODE
    //
    // The connection task owns EVERYTHING here: the tick, the keyframe cursor,
    // the keyframe collection itself, the emitter, and the socket. MFP's event
    // thread only raises flags (_segNeedsResync / _segPlaying) from the
    // HandleMessage overrides below — it reads no state and writes no data.
    //
    // WHY IT IS A TICK AND NOT A MESSAGE HANDLER (the v0.2.1 fix). Until 0.2.0
    // the emitter ran inside HandleMessage(MediaPositionChangedMessage) and
    // emitted only the span the cursor was standing in, once. That makes the
    // media source's position-report rate the segment rate, and those rates are
    // not remotely the same number:
    //   * MFP's own script engine runs at Script/UpdateInterval — 1 ms on this
    //     rig — advancing an INTERNAL media clock (ScriptViewModel
    //     ._internalMediaPosition) every tick. That is what makes "Axis::
    //     Position" a ~1 kHz-fresh value and what makes Samples mode smooth.
    //   * MediaPositionChangedMessage is only the media source's CORRECTION to
    //     that internal clock. The Web (WebView2) source posts one per HTML5
    //     'timeupdate' event — Chromium fires those about every 250 ms, i.e.
    //     ~4 Hz. Other sources differ, and none of them promise anything.
    // A funscript runs 2–6 actions/s typically and 20+ in dense sections, so a
    // 4 Hz emitter dropped most of the script on the floor and sent the machine
    // at whatever keyframe happened to be next when the browser felt chatty.
    // The machine was faithfully rendering a decimated script.
    //
    // So: poll the 1 kHz-fresh position at SegTickMs and emit every span as it
    // comes due. Spans are SCHEDULED, not fired blind — see SendSegmentAsync.
    //
    // The loop also interleaves three timers: a silence-based PING keepalive
    // (segments are sparse — without it the hub's 600 ms deadman would fire
    // between strokes), the usual ~10 s CLOCK resync, and a ~1 s
    // output-divergence probe.
    // =========================================================================
    private const double SegmentPingIntervalMs = 400.0;   // < 600 ms deadman, with margin
    // 0x0085 publish wish (Hz). Becomes the hub's §10.5 sustained token-bucket
    // ceiling AND its burst depth — 30 clears the ~25 Hz worst-case dense-section
    // burst with headroom, under the channel's 50 Hz cap. See HELLO in SessionAsync.
    private const double SegmentWishHz = 30.0;

    // Emitter tick. 10 ms is two orders of magnitude under the shortest span a
    // 50 Hz-capped channel can carry, so no span can slip past unseen; the
    // lookahead below absorbs whatever jitter Task.Delay actually delivers.
    private const int SegTickMs = 10;

    // How far ahead of the media clock a span may be handed to the device. The
    // segment's START rides on the bundle's t_base (§5.4 pins t_off[0] to 0 and
    // caps the bundle span at 20 ms, so scheduling CANNOT ride on t_off), and
    // the hub clamps a wire timestamp more than 250 ms in the future — stay well
    // under that. This is also the exposure window for stale motion after a seek
    // or a pause: whatever is already scheduled still plays out.
    private const double SegLookaheadMs = 120.0;

    // Media-clock discontinuity thresholds (seconds of SCRIPT time).
    // Forward: bigger than this between ticks means a seek/stall, not playback —
    // the spans in between are historical and must be dropped, never replayed.
    // Backward: MFP nudges its internal clock backwards by a few ms whenever a
    // media-source correction lands, which is normal and must not trigger a
    // re-anchor; a real rewind (or a media loop wrapping to 0) is far bigger.
    private const double SegSeekForwardSec = 0.25;
    private const double SegSeekBackwardSec = 0.05;

    // How long the media clock must stand still before we call it "paused". This
    // is the ONLY thing that stops the emitter — see the liveness note in
    // SegTickAsync. Long enough to be hysteresis for a coarse MFP update
    // interval, short enough that a pause stops motion promptly.
    private const double SegFrozenIdleMs = 60.0;

    // Heartbeat cadence: the decision-state line goes out every second, at Info
    // while nothing is being emitted and at Info once per SegBeatInfoEveryMs
    // while healthy (Debug in between — MFP ships at LogLevel=Info).
    private const double SegBeatIntervalMs = 1000.0;
    private const double SegBeatInfoEveryMs = 10_000.0;

    // Per-tick emit budget. Normal playback needs 0 or 1; a burst only happens
    // when a whole lookahead window comes due at once (session start, resync,
    // dense section). Anything beyond this is a symptom, not a workload.
    private const int SegMaxEmitPerTick = 4;
    private const int SegMaxScanPerTick = 4096;   // guards the past-span skip walk

    // Wire duration floor. The channel itself only rejects 0, but a segment far
    // shorter than the move it commands is an infeasible order — the device
    // would have to slam. We never generate one now (a span is only emitted
    // while its end is still in the future), so this is a backstop.
    private const int SegMinDurationMs = 10;

    // End-velocity handoff limiter (see ScriptSlopeAtSpanEnd for the full
    // derivation and the measured numbers). A knot tangent is capped at
    //     |v_end| <= SegHandoffChordFactor * min(|chord_in|, |chord_out|)
    // 1.5 is not a fudge: capping BOTH endpoint tangents of a span at k*|chord|
    // makes the Fritsch-Carlson sum condition alpha + beta <= 2k, and 2k <= 3 —
    // the classic sufficient condition for a shape-preserving (non-overshooting)
    // Hermite — exactly at k = 1.5. Raise toward 3.0 for the looser per-tangent
    // box if the machine ends up flatter than the script through long shallow
    // runs; this is the ONE knob for handoff aggressiveness.
    private const double SegHandoffChordFactor = 1.5;

    // *** REMOVAL FLAG — flip to false to delete the limiter's effect. ***
    //
    // RFC-008 says the MACHINE owns motion processing, so a client-side shape
    // limiter is the wrong side of the line on principle.
    //
    // THE HUB GUARD HAS NOW LANDED (fw 2.1.53 / slopmotion 0.7.0, milestone
    // M4d): SlopMotion bounds an inbound segment's end velocity against the
    // FOLLOWING segment's chord using this exact Fritsch–Carlson rule, with
    // the lookahead taken from the SlopSync pacing ring, and reports every
    // bounded handoff as a `handoff_bounded` anomaly (0x0089 EVENT + 0x0088
    // counter + /api/slopmotion). Machine-side k is live-tunable via
    // POST /api/slopmotion {"handoff_k": ...}, where 0 disables it.
    //
    // THIS FLAG IS THEREFORE NOW SAFE TO FLIP TO FALSE — that is milestone
    // M5d's A/B: run a Makima axis against slopsim (or the machine) with
    // client limiter ON/hub k=0, client OFF/hub k=1.5, and both, and compare.
    // DELETION waits on that comparison, not on the guard existing. One caveat
    // worth knowing before you read the results: the hub can only bound a
    // handoff when the NEXT segment is already in its pacing ring, which
    // requires the successor to have been scheduled before its predecessor came
    // due — with SegLookaheadMs = 120 that holds for spans shorter than
    // ~120 ms and not for longer ones. Raising SegLookaheadMs (the wire allows
    // scheduling up to 250 ms ahead before the hub clamps t_off) widens the
    // guard's coverage and is the obvious knob to sweep in the same session.
    private const bool SegHandoffLimiterEnabled = true;

    private async Task SegmentLoopAsync(HubClient client, double sampleRate, double segRate, CancellationToken token)
    {
        // Fresh engine state for this session.
        _segAxis = DeviceAxis.Parse(SourceAxis);
        _segPendingKeyframes = null;   // never inherit a previous session's/axis' script
        _segKeyframes = ReadKeyframes(_segAxis);
        _segEmittedSpan = -1;
        _segHaveAxisPos = false;
        _segNeedsResync = true;
        _segThrottled = 0;
        _segThrottleLogAgeMs = 1e9;   // first throttle event logs immediately
        _segCacheAgeMs = 1e9;         // first tick refreshes the transform cache
        _segFrozenMs = 0;
        _segStop = "starting";
        _segStopWarned = null;
        _segBeatSegs = 0;
        _segBeatAtMs = 0;
        _segBeatPos = double.NaN;
        _segBeatKfCount = -1;
        try { _segPlaying = ReadProperty<bool>("Media::PlayPause"); } catch { _segPlaying = false; }

        // Token bucket sized off the ACTUAL grant (ground truth from WELCOME),
        // not off our wish — the hub may have granted less than we asked for.
        _segTokenRate = segRate > 0.5 ? segRate : SegmentWishHz;
        _segTokenDepth = _segTokenRate;
        _segTokens = _segTokenDepth;

        Ui(() => { SegmentsSent = 0; DivergenceWarning = null; });
        _segActive = true;   // opens the gate for the HandleMessage flag setters

        var sw = Stopwatch.StartNew();
        var connectedAt = DateTime.UtcNow;
        long localSegs = 0;
        double lastSendMs = 0;      // last time ANY frame went out (segment/ping/clock)
        double lastTickMs = 0;      // for the token-bucket refill
        double lastResyncMs = 0;
        double lastStatsMs = 0;
        double lastDivergeMs = 0;
        double lastBeatInfoMs = -SegBeatInfoEveryMs;   // first beat always logs at Info
        int divergeStreak = 0;

        try
        {
            while (!token.IsCancellationRequested)
            {
                double nowMs = sw.Elapsed.TotalMilliseconds;
                double dtMs = nowMs - lastTickMs;
                lastTickMs = nowMs;

                // ---- operator intents (Home / stroke window) ----------------
                // Same task, same socket-ownership rule as the emitter below.
                await DrainIntentsAsync(client, token);
                await PumpCatalogReadyAsync(client, nowMs, token);

                // ---- the emitter -------------------------------------------
                int sent = await SegTickAsync(client, dtMs, token);
                if (sent > 0)
                {
                    localSegs += sent;
                    lastSendMs = sw.Elapsed.TotalMilliseconds;
                }

                nowMs = sw.Elapsed.TotalMilliseconds;

                // ---- PING keepalive (only when otherwise silent) ------------
                if (nowMs - lastSendMs >= SegmentPingIntervalMs)
                {
                    await client.SendPingAsync(token);
                    lastSendMs = nowMs;
                }

                // ---- periodic CLOCK resync (~10 s) --------------------------
                if (nowMs - lastResyncMs >= SlopWire.ClockResyncIntervalMs)
                {
                    lastResyncMs = nowMs;
                    await ResyncClock(client, token);
                    lastSendMs = sw.Elapsed.TotalMilliseconds;   // CLOCK is traffic too
                }

                // ---- output-divergence probe (~1 s) -------------------------
                if (nowMs - lastDivergeMs >= 1000)
                {
                    lastDivergeMs = nowMs;
                    CheckDivergence(ref divergeStreak);
                }

                // ---- decision-state heartbeat (1 Hz) ------------------------
                if (nowMs - _segBeatAtMs >= SegBeatIntervalMs)
                {
                    _segBeatAtMs = nowMs;
                    bool infoTick = nowMs - lastBeatInfoMs >= SegBeatInfoEveryMs;
                    if (infoTick) lastBeatInfoMs = nowMs;
                    SegHeartbeat(localSegs, infoTick);
                }

                // ---- throttled stats push -----------------------------------
                if (nowMs - lastStatsMs >= 200)
                {
                    lastStatsMs = nowMs;
                    long segSnapshot = localSegs;
                    var up = DateTime.UtcNow - connectedAt;
                    Ui(() =>
                    {
                        SegmentsSent = segSnapshot;
                        Uptime = $"{(int)up.TotalMinutes:D2}:{up.Seconds:D2}";
                    });
                }

                try { await Task.Delay(SegTickMs, token); }
                catch (OperationCanceledException) { break; }
            }
        }
        finally
        {
            _segActive = false;
        }
    }

    // ---- Keyframe read (null-safe) ------------------------------------------
    // THE PROPERTY IS "Axis::SelectedScript". There is no "Axis::Script" — that
    // name was wrong from the first version of this plugin and returned null on
    // every call. It went unnoticed for two releases because the old emitter also
    // took keyframes from ScriptChangedMessage, which quietly carried the whole
    // feature; the moment 0.2.1 made this the only source, Segments mode went
    // stone dead with no log line to show for it.
    //
    // Verified against the shipped assembly, not guessed: MFP registers its
    // plugin-readable properties through IPropertyManager.RegisterProperty, and
    // an IL scan of every RegisterProperty call site in
    // ScriptViewModel.RegisterProperties lists
    //   Axis::SelectedScript  ->  RegisterProperty<DeviceAxis, ScriptResource>
    // with no "Axis::Script" anywhere in the assembly (the near-misses are
    // Axis::ScriptScale / ScriptValue / ScriptOffset / Scripts / Bypass::Script).
    // 1.34.5 exposes the concrete ScriptResource (there is no IScriptResource
    // interface in that build); ScriptResource.Keyframes is the collection.
    //
    // The message-delivered copy stays as a fallback: belt and braces, since a
    // property name is exactly the kind of thing that moves between MFP builds.
    private KeyframeCollection ReadKeyframes(DeviceAxis axis)
    {
        if (axis == null) return null;
        try
        {
            var kf = ReadProperty<DeviceAxis, ScriptResource>("Axis::SelectedScript", axis)?.Keyframes;
            if (kf != null && kf.Count >= 2) return kf;
        }
        catch (Exception ex) { Logger.Debug(ex, "Axis::SelectedScript read failed"); }
        return _segPendingKeyframes;
    }

    // ---- Transform replication (ScriptScale + InvertScript) -----------------
    // Reproduces the two cheap deterministic stages ScriptViewModel applies to a
    // raw keyframe value (verified against ScriptViewModel.cs UpdateScript):
    //   value = Clamp01(default + (value - default) * ScriptScale); if invert 1-value.
    // Deeper stages (motion-provider blend, SmartLimit, SpeedLimit, sync/autohome)
    // cannot be replicated here — the divergence probe catches those at runtime.
    // Scale/invert come from the per-tick snapshot (RefreshTransformCache), not a
    // fresh property read, so every value in one tick goes through one transform.
    private double TransformValue(DeviceAxis axis, double raw)
    {
        double def = axis?.DefaultValue ?? 0.5;
        double v = Clamp01(def + (raw - def) * _segScale);
        return _segInvert ? 1.0 - v : v;
    }

    private void RefreshTransformCache()
    {
        try { _segScale = ReadProperty<DeviceAxis, double>("Axis::ScriptScale", _segAxis); } catch { _segScale = 1.0; }
        try { _segInvert = ReadProperty<DeviceAxis, bool>("Axis::InvertScript", _segAxis); } catch { _segInvert = false; }
        try { _segInterp = ReadProperty<DeviceAxis, InterpolationType>("Axis::InterpolationType", _segAxis); }
        catch { _segInterp = InterpolationType.Linear; }
        if (double.IsNaN(_segScale) || _segScale <= 0) _segScale = 1.0;
    }

    private static double Clamp01(double v) => v < 0 ? 0 : (v > 1 ? 1 : v);

    private double ReadSpeed()
    {
        try { double s = ReadProperty<double>("Media::Speed"); return s > 1e-3 ? s : 1.0; }
        catch { return 1.0; }
    }

    // ---- The cursor / emit core (all on the CONNECTION task) ----------------
    // One tick: sample the media clock, notice discontinuities, then hand the
    // device every span that has come due since the last tick. Returns how many
    // segments actually went out (the caller uses it for the PING silence timer).
    private async Task<int> SegTickAsync(HubClient client, double dtMs, CancellationToken token)
    {
        if (_segAxis == null) return SegStop("source axis '" + SourceAxis + "' did not parse");
        _segCacheAgeMs += Math.Max(0, dtMs);
        _segThrottleLogAgeMs += Math.Max(0, dtMs);

        // Axis::Position is the axis-local SCRIPT time in seconds (media position
        // through the axis' own offset) — the same value MFP indexes its own
        // keyframe cursor with, and the one ScriptViewModel's 1 kHz update thread
        // refreshes (Script/UpdateInterval). This is the ONLY input the emitter
        // needs, deliberately: every other property read in this file is either a
        // setting or a diagnostic, so no property lookup can stop motion.
        double axisPos;
        try { axisPos = ReadProperty<DeviceAxis, double>("Axis::Position", _segAxis); }
        catch (Exception ex)
        {
            Logger.Debug(ex, "Axis::Position read threw");
            return SegStop("Axis::Position unreadable for '" + SourceAxis + "'");
        }
        if (double.IsNaN(axisPos)) return SegStop("Axis::Position is NaN");
        _segBeatPos = axisPos;

        double speed = ReadSpeed();

        // Discontinuity detection on the tick, not on the media message: a jump
        // forward means spans went by while we were not looking (stall / seek)
        // and a jump backward means a rewind or a media loop wrap. Either way the
        // cursor is re-anchored and the spans in between are DROPPED — their time
        // is already past, and replaying them would drag the machine behind the
        // media instead of catching it up.
        bool jump = !_segHaveAxisPos
                    || (axisPos - _segLastAxisPos) > SegSeekForwardSec
                    || (_segLastAxisPos - axisPos) > SegSeekBackwardSec;

        // LIVENESS IS POSITION ADVANCEMENT, NOT A "playing" FLAG. 0.2.1 gated the
        // emitter on a Media::PlayPause read and that gate failed CLOSED and
        // SILENT. A gate in this loop must fail toward emitting, so the only thing
        // that stops us is the media clock itself standing still — which is what
        // "paused" physically means, needs no property, and cannot lie. A scrub
        // while paused MOVES the clock, so it still re-anchors and still emits,
        // which is exactly what MFP's own axis does. The 60 ms window is hysteresis
        // for an MFP configured with a coarse Script/UpdateInterval (1 ms here).
        if (!_segHaveAxisPos || Math.Abs(axisPos - _segLastAxisPos) > 1e-9) _segFrozenMs = 0;
        else _segFrozenMs += Math.Max(0, dtMs);

        _segLastAxisPos = axisPos;
        _segHaveAxisPos = true;

        if (jump || _segNeedsResync)
        {
            // A re-anchor is also where the keyframes and the settings snapshot
            // are re-pulled, and it MUST be the tick that pulls them: if the MFP
            // thread swapped the collection itself, a tick that had already read
            // the old reference could walk the new script with a stale index and
            // emit one span from the wrong place. Every message that can change
            // the script raises this flag instead, and the pull happens here,
            // with the cursor reset in the same breath.
            _segNeedsResync = false;
            _segEmittedSpan = -1;   // -1 = "find me from the position again"
            _segCacheAgeMs = 0;
            RefreshTransformCache();

            var fresh = ReadKeyframes(_segAxis);
            _segKeyframes = fresh;
            if (fresh == null || fresh.Count < 2)
            {
                _segNeedsResync = true;   // script still loading — retry next tick
                _segBeatKfCount = fresh?.Count ?? -1;
                return SegStop("no script loaded on " + SourceAxis + " (Axis::SelectedScript empty)");
            }
        }
        else if (_segCacheAgeMs >= 250)
        {
            // ScriptScale / InvertScript / InterpolationType are settings, not
            // live signals — a few reads a second, not a hundred.
            _segCacheAgeMs = 0;
            RefreshTransformCache();
        }

        var kf = _segKeyframes;
        _segBeatKfCount = kf?.Count ?? -1;
        if (kf == null || kf.Count < 2)
        {
            _segNeedsResync = true;   // force a fresh pull next tick
            return SegStop("keyframe collection went away mid-session");
        }

        if (_segFrozenMs >= SegFrozenIdleMs) return SegIdle("paused (media clock frozen)");

        // Token bucket refill (client-side mirror of the hub's §10.5 bucket).
        _segTokens = Math.Min(_segTokenDepth, _segTokens + _segTokenRate * Math.Max(0, dtMs) / 1000.0);

        // Spans are emitted when their START enters the lookahead window, so the
        // horizon is in SCRIPT seconds — a wall-clock lookahead scaled by the
        // playback speed (at 2x, 120 ms of wall clock is 240 ms of script).
        double horizonScript = axisPos + (SegLookaheadMs / 1000.0) * speed;

        int i = _segEmittedSpan >= 0 ? _segEmittedSpan + 1 : kf.SearchForIndexBefore(axisPos);
        if (i < 0) i = 0;   // before the script starts: wait at span 0 until it comes due

        int sent = 0;
        int scanned = 0;
        string outcome = i + 1 < kf.Count ? "waiting for the next action" : "past the end of the script";
        for (; i + 1 < kf.Count && sent < SegMaxEmitPerTick && scanned < SegMaxScanPerTick; i++, scanned++)
        {
            if (kf[i].Position > horizonScript)                 // not due yet
            {
                outcome = $"next action at t={kf[i].Position:F2}s, {(kf[i].Position - axisPos):F2}s away";
                break;
            }

            // A gap span carries no motion — the machine holds wherever the
            // previous segment left it (the segment BEFORE a gap is always sent
            // with the sentinel, so the engine settles rather than coasting).
            // A span whose end is already behind the media clock is history.
            if (!kf.IsGap(i) && kf[i + 1].Position > axisPos)
            {
                if (_segTokens < 1.0)
                {
                    // Out of budget: leave the cursor BEFORE this span so the
                    // next tick retries it rather than silently dropping motion.
                    _segThrottled++;
                    if (_segThrottleLogAgeMs >= 2000)
                    {
                        _segThrottleLogAgeMs = 0;
                        Logger.Warn("Segments mode: shaping to the {0:F0} Hz grant ({1} deferred so far) — this script section has more actions/s than the channel grant",
                                    _segTokenRate, _segThrottled);
                    }
                    outcome = $"rate-shaped at {_segTokenRate:F0} Hz ({_segThrottled} deferred)";
                    break;
                }
                _segTokens -= 1.0;
                await SendSegmentAsync(client, kf, i, axisPos, speed, token);
                sent++;
            }

            _segEmittedSpan = i;
        }

        if (sent > 0)
        {
            // Motion is flowing — clear the "already warned" latch so a NEW stall
            // later on gets its own warning instead of being swallowed.
            _segStop = null;
            _segStopWarned = null;
            return sent;
        }
        return SegIdle(outcome);
    }

    // Records why a tick produced nothing. SegIdle is for the normal quiet states
    // (paused, between actions, past the end); SegStop is for the ones that mean
    // something is wrong, and warns ONCE per distinct reason until motion flows
    // again. Both return 0 so every early return in the emitter reads as
    // `return SegStop(...)` — there is no silent path left.
    private int SegIdle(string reason) { _segStop = reason; return 0; }

    private int SegStop(string reason)
    {
        _segStop = reason;
        if (_segStopWarned != reason)
        {
            _segStopWarned = reason;
            Logger.Warn("Segments mode: NOT EMITTING — {0}", reason);
        }
        return 0;
    }

    // 1 Hz decision-state dump. This exists because "no segments and no log line"
    // is the worst failure this plugin can have: every field below is one of the
    // inputs the emitter actually branches on, so a single line names the fault.
    //   play  — Media::PlayPause (DIAGNOSTIC ONLY; the emitter does not gate on it)
    //   pos   — Axis::Position, the script clock the emitter runs on
    //   kf    — keyframe count (-1 = no script; this was the 0.2.1 killer)
    //   span  — our cursor / MFP's own cursor (Axis::Index); they should track
    //   tok   — token-bucket credit
    //   sent  — segments sent since the previous beat
    //   why   — the last tick's non-emitting reason ("-" while emitting)
    // Logged at Info while stalled (once a second) and every 10 s while healthy —
    // MFP ships at LogLevel=Info, so Debug would be invisible exactly when needed.
    // The same line is appended to the plugin's StatusText, always visible.
    private void SegHeartbeat(long segsTotal, bool infoTick)
    {
        long delta = segsTotal - _segBeatSegs;
        _segBeatSegs = segsTotal;

        bool play; try { play = ReadProperty<bool>("Media::PlayPause"); } catch { play = _segPlaying; }
        string mfpIdx; try { mfpIdx = ReadProperty<DeviceAxis, int>("Axis::Index", _segAxis).ToString(); } catch { mfpIdx = "?"; }

        string beat = $"play={(play ? 1 : 0)} pos={_segBeatPos:F2}s kf={_segBeatKfCount} " +
                      $"span={_segEmittedSpan}/{mfpIdx} tok={_segTokens:F0} sent={delta}/s why={_segStop ?? "-"}";

        if (delta == 0 || infoTick) Logger.Info("segbeat {0}", beat);
        else Logger.Debug("segbeat {0}", beat);

        string status = _segStatusBase == null ? beat : _segStatusBase + "  ·  " + beat;
        Ui(() => StatusText = status);
    }

    // One span → one 0x0085 sample. Two cases, one rule:
    //   * a span we have NOT entered yet (the normal case — the lookahead sees it
    //     coming) is SCHEDULED at its true start with its FULL duration. The
    //     start time rides on the bundle's t_base, because §5.4 pins t_off[0] to
    //     0 and caps the bundle span at 20 ms; the hub resolves t_base+t_off
    //     against its own clock and honours it as the segment START. This is what
    //     makes segment timing independent of our tick jitter.
    //   * a span we are already INSIDE (right after a re-anchor, or if a tick ran
    //     late) is planned from NOW to its unchanged end. Shortening the duration
    //     rather than shifting the endpoint keeps the machine in phase with the
    //     media instead of accumulating lag.
    private Task SendSegmentAsync(HubClient client, KeyframeCollection kf, int i,
                                  double axisPos, double speed, CancellationToken token)
    {
        double startScript = kf[i].Position;
        double endScript = kf[i + 1].Position;

        double aheadSec = (startScript - axisPos) / speed;
        double durSec = aheadSec > 0 ? (endScript - startScript) / speed
                                     : (endScript - axisPos) / speed;

        int durMs = (int)Math.Clamp(Math.Round(durSec * 1000.0), SegMinDurationMs, 65535);
        uint offUs = (uint)Math.Clamp(Math.Round(Math.Max(0, aheadSec) * 1_000_000.0), 0, SegLookaheadMs * 1000.0);

        double target = TransformValue(_segAxis, kf[i + 1].Value);
        var (endVel, sentinel) = ComputeEndVel(kf, i, speed);

        Ui(() => LastTarget = target);
        return client.SendSegmentSampleAsync(unchecked(client.HubNowUs() + offUs),
                                             new SegmentSample(target, durMs, endVel, sentinel), token);
    }

    // Outgoing-slope continuity for the end-velocity handoff (§0x0085 semantics):
    //   * no keyframe after the target, or the outgoing span is a gap → SENTINEL
    //     (unconstrained — the engine settles / holds; these are the two cases
    //     where "the span after the target" has no real slope to hand off).
    //   * otherwise → the SCRIPT'S OWN TANGENT at the target keyframe, limited to
    //     a feasible handoff (ScriptSlopeAtSpanEnd) and clamped ±32.767 norm/s.
    //     0 is a real slope here, not "absent".
    // The structural (gap/absent) check must precede the kinematic one because the
    // outgoing slope is undefined without a valid, non-gap outgoing span — and
    // because ScriptSlopeAtSpanEnd's limiter reads kf[i+2], which those two
    // guards are what make safe.
    private (double endVel, bool sentinel) ComputeEndVel(KeyframeCollection kf, int i, double speed)
    {
        int j = i + 1;   // outgoing span index (span after the target keyframe kf[i+1])
        if (j + 1 >= kf.Count) return (0, true);      // no kf[i+2] → end of script → SENTINEL
        if (kf.IsGap(j)) return (0, true);             // outgoing gap → hold → SENTINEL

        // value / SCRIPT-second → value / WALL-second, then through the same
        // affine transform the targets take. d/dv of Clamp01(def+(v-def)*scale)
        // is just scale, and InvertScript negates it. (If ScriptScale pushes the
        // curve into the 0/1 clamp the true derivative there is 0 — a corner case
        // we do not chase; the device clamps to the window regardless.)
        double endVel = ScriptSlopeAtSpanEnd(kf, i) * speed * _segScale * (_segInvert ? -1.0 : 1.0);
        if (double.IsNaN(endVel) || double.IsInfinity(endVel)) return (0, false);

        return (Math.Clamp(endVel, -32.767, 32.767), false);
    }

    // The derivative of MFP's OWN interpolant at the end of span i, in value per
    // script-second, LIMITED to a handoff the device's quintic can actually
    // honour. KeyframeCollection.CalculateSlopes(i, type) returns the pair of
    // endpoint tangents for span i; Item2 is the one at kf[i+1], and for the C1
    // spline types it equals the incoming tangent of the next span, which is
    // exactly the handoff the device needs.
    //
    // UNITS — MEASURED, not assumed (scratch harness against MultiFunPlayer.dll
    // 1.34.5, comparing CalculateSlopes to a 5-point numeric derivative of
    // Interpolate() taken just inside each end of the span):
    //   Item1/num'@start = Item2/num'@end = 1.000 for Pchip and Makima, over
    //   span durations from 0.1 s to 2.0 s, and time-scaling a script x4 divides
    //   every slope by 4. So CalculateSlopes is value per SCRIPT-SECOND — NOT a
    //   Hermite tangent per unit of normalized span parameter. Item2 really is
    //   the tangent at kf[i+1] (it equals Item1 of span i+1 everywhere except the
    //   extrapolated first span). Multiplying by `speed` is therefore the whole
    //   script->wall conversion; there is no span-duration factor to apply. The
    //   `* _segScale` in ComputeEndVel is likewise exact: TransformValue is the
    //   affine Clamp01(def + (raw-def)*scale), whose derivative in raw is just
    //   `scale` (MFP's ScriptScale is a ratio, default 1.0 — not a percentage).
    //
    // WHY THE LIMITER (the flat-top bug). A raw spline tangent is the derivative
    // of a curve MFP draws with no kinematic ceiling anywhere; the device has to
    // realise it as ONE quintic that covers the span's displacement in the span's
    // duration while ARRIVING at that velocity. When the tangent dwarfs the
    // span's mean velocity, no monotone quintic exists — SlopMotion's legality
    // scan rejects it (WaveformFallback) and Ruckig re-shapes the segment into a
    // bang-cruise-bang profile, which is precisely a straight line in position
    // with a FLAT-TOPPED velocity trace. Worse, the device estimates the segment's
    // end ACCELERATION as a backward difference of consecutive end_vel values, so
    // one oversized tangent poisons the next segment's boundary too.
    //
    // That failure is structurally a SAME-DIRECTION phenomenon, which is exactly
    // what the field reported (runs misbehave, reversals are fine). Akima-family
    // tangents are a weighted blend of the two adjoining chord slopes:
    //   * at a REVERSAL the chords straddle zero, so the blend lands between them
    //     and |d| is bounded by the larger |chord| — measured 1.47 between chords
    //     of +4.79 and -2.40. Big, but nothing the span cannot absorb.
    //   * on a same-direction RUN the chords are same-signed, so the blend lands
    //     between them — and "between 3.00 and 0.05" is 1.82, i.e. 36x the mean
    //     velocity of the shallow span it has to be delivered into. Measured
    //     against the Fritsch-Carlson knot bound 3*min(|chord|): Makima 12.1x
    //     over on that knot, and >1.0x even on a mild uneven-spacing rise.
    // Pchip is monotonicity-preserving by construction and its tangents satisfy
    // the same bound (measured max 0.89x, and exactly 0 at every extremum), which
    // is why switching the axis to Pchip was "much much better" — it dodges the
    // pathology instead of fixing it, and its remaining 0.89x headroom is why it
    // was not perfect. The limiter below fixes both.
    //
    // Measured effect of the limiter (k = 1.5, Makima end_vel per knot): the
    // steep/shallow run above goes 1.816 -> 0.075 and 1.563 -> 0.075 (from 31x
    // the shallow span's mean velocity down to 1.5x); a normal stroke script is
    // essentially untouched, because on Pchip every reversal knot is already 0
    // and on a run with similar adjoining chords the tangent is already ~1x the
    // chord. Sharp reversals whose neighbouring spans differ a lot in LENGTH do
    // see a modest trim (measured 2.008 -> 1.441 on a 0.167 s / 0.833 s zig-zag);
    // that is the limiter working as intended, not a regression — the long span
    // genuinely cannot absorb the short one's speed.
    //
    // CalculateSlopes returns NaN for Linear (there is no spline to differentiate)
    // and Step has no meaningful slope at all — both fall back to the chord, which
    // for Linear IS the true derivative. The limiter applies to that too: a Linear
    // corner into a much shallower span is the same infeasible order.
    private double ScriptSlopeAtSpanEnd(KeyframeCollection kf, int i)
    {
        double slope;
        if (_segInterp == InterpolationType.Step)
        {
            slope = 0;   // held value; the jump is at the keyframe
        }
        else
        {
            slope = double.NaN;
            if (_segInterp != InterpolationType.Linear)
            {
                try
                {
                    var (_, sEnd) = kf.CalculateSlopes(i, _segInterp);
                    if (!double.IsNaN(sEnd) && !double.IsInfinity(sEnd)) slope = sEnd;
                }
                catch { /* unknown interpolation type on some MFP build — chord below */ }
            }
            if (double.IsNaN(slope))
            {
                double dt0 = kf[i + 1].Position - kf[i].Position;
                slope = dt0 > 1e-9 ? (kf[i + 1].Value - kf[i].Value) / dt0 : 0.0;
            }
        }

        return LimitHandoffSlope(kf, i, slope);
    }

    // Fritsch-Carlson knot limiter, in script units (the chords and the tangent
    // are both value/script-second, so limiting here and scaling afterwards is
    // the same thing as limiting after — one fewer place to get the units wrong).
    //
    // The tangent at knot kf[i+1] is the END of span i and the START of span i+1,
    // so it has to be feasible for BOTH: cap it at k*min of the two chord speeds.
    // A zero chord on either side (plateau, or the target IS the next keyframe's
    // value) forces 0 — arriving at a hold still moving is exactly the kind of
    // lie the ground-truth rule exists to prevent. The cap is on MAGNITUDE only:
    // a wrong-signed tangent stays wrong-signed but bounded, so the only thing
    // this changes is pathological size. ComputeEndVel has already established
    // that span i+1 exists and is not a gap.
    private double LimitHandoffSlope(KeyframeCollection kf, int i, double slope)
    {
        if (double.IsNaN(slope) || double.IsInfinity(slope)) return 0.0;
#pragma warning disable CS0162 // unreachable while the flag is const-true — that is the point
        if (!SegHandoffLimiterEnabled) return slope;
#pragma warning restore CS0162

        double dtIn = kf[i + 1].Position - kf[i].Position;
        double dtOut = kf[i + 2].Position - kf[i + 1].Position;
        if (!(dtIn > 1e-9) || !(dtOut > 1e-9)) return 0.0;   // degenerate spacing — no honest handoff

        double mIn = Math.Abs((kf[i + 1].Value - kf[i].Value) / dtIn);
        double mOut = Math.Abs((kf[i + 2].Value - kf[i + 1].Value) / dtOut);
        double limit = SegHandoffChordFactor * Math.Min(mIn, mOut);

        if (Math.Abs(slope) <= limit) return slope;
        return slope < 0 ? -limit : limit;
    }

    // Runtime divergence probe (connection task): compare the axis' ACTUAL output
    // against what we predict the authored+transformed script to be at the current
    // position. Persistent divergence means a stage we can't replicate (motion
    // provider, SmartLimit, sync, …) is active — warn, never switch modes.
    private void CheckDivergence(ref int streak)
    {
        var kf = _segKeyframes;
        if (_segAxis == null || kf == null || kf.Count < 2)
        {
            streak = 0;
            if (DivergenceWarning != null) Ui(() => DivergenceWarning = null);
            return;
        }

        try
        {
            double pos = ReadProperty<DeviceAxis, double>("Axis::Position", _segAxis);
            int idx = kf.SearchForIndexBefore(pos);
            if (idx < 0 || idx + 1 >= kf.Count) { streak = 0; return; }   // outside script — undefined, skip

            double predicted = TransformValue(_segAxis, Clamp01(kf.Interpolate(idx, pos, _segInterp)));
            double actual = ReadProperty<DeviceAxis, double>("Axis::Value", _segAxis);

            if (Math.Abs(actual - predicted) > 0.05) streak++;
            else streak = 0;

            if (streak >= 3)
                Ui(() => DivergenceWarning = "Axis output diverges from script (motion provider / smart-limit active?) — Segments mode is sending the authored script; consider Samples mode.");
            else if (streak == 0 && DivergenceWarning != null)
                Ui(() => DivergenceWarning = null);
        }
        catch (Exception ex) { Logger.Debug(ex, "divergence probe read failed"); }
    }

    // =========================================================================
    // MFP message hooks — FLAG SETTERS ONLY. The emitter is SegTickAsync on the
    // connection task; nothing here walks keyframes or touches the socket. All
    // no-op unless a Segments session is live (_segActive). These run on MFP's
    // event thread, which delivers messages serially.
    // =========================================================================
    protected override void HandleMessage(MediaPositionChangedMessage message)
    {
        // This is NOT the emitter clock (it used to be — see the SEGMENTS MODE
        // header). It is the media source's correction to MFP's internal media
        // clock, and its rate is whatever the source feels like: the Web/WebView2
        // source posts one per HTML5 'timeupdate', which Chromium throttles to
        // ~4 Hz. All we take from it is MFP's explicit "that was a seek" flag —
        // the tick's own discontinuity detector catches everything else, and
        // catches it against the 1 kHz-fresh Axis::Position rather than this.
        if (_segActive && message.ForceSeek) _segNeedsResync = true;
    }

    protected override void HandleMessage(MediaPlayingChangedMessage message)
    {
        _segPlaying = message.IsPlaying;
        // Resume: re-anchor on the next tick (≤10 ms) from the real position.
        // The tick reads Media::PlayPause itself, so this only saves it from
        // planning one stale span if the property lags the message.
        if (_segActive && message.IsPlaying) _segNeedsResync = true;
    }

    protected override void HandleMessage(MediaSpeedChangedMessage message)
    {
        // A speed change rescales every segment's wall-clock duration — force a
        // fresh plan on the next tick. Segments already SCHEDULED inside the
        // lookahead window keep their old durations; that is ≤120 ms of stale
        // pacing, which the device rides out and the next plan corrects.
        if (_segActive) _segNeedsResync = true;
    }

    protected override void HandleMessage(ScriptChangedMessage message)
    {
        // Park the collection in _segPendingKeyframes and raise the flag — the
        // TICK decides when to adopt it (see the re-anchor block in SegTickAsync).
        // Writing _segKeyframes straight from this thread would let a tick that
        // had already read the old reference walk the new script with a stale
        // cursor. The parked copy is a fallback for the Axis::SelectedScript read.
        if (!_segActive || _segAxis == null || message.Axis != _segAxis) return;
        _segPendingKeyframes = message.Script?.Keyframes;
        _segNeedsResync = true;
    }

    protected override void HandleMessage(PostScriptSearchMessage message)
    {
        // Batched refresh signal. The message payload shape differs across MFP
        // versions (1.34.5 carries a Result, master a Scripts map), so we never
        // read it — the tick re-pulls the keyframes straight from the axis
        // property, which is version-proof and always current.
        if (!_segActive || _segAxis == null) return;
        _segNeedsResync = true;
    }

    protected override void HandleMessage(MediaSeekMessage message)
    {
        // Secondary (MFP-internal) seek signal — the tick's own discontinuity
        // detector is primary; this just guarantees a re-anchor even for a scrub
        // too small to trip the ±0.25 s / ±0.05 s thresholds.
        if (_segActive) _segNeedsResync = true;
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
// StreamMode — how the plugin feeds the machine.
//   Samples  — the original 50 Hz dense-point path on 0x0084 (motion-input).
//   Segments — one timed {target, duration, end_vel} per funscript action on
//              0x0085 (motion-segment); ~2–4 packets/s, the device renders the
//              native waveform. 0x0084 stays granted as a fallback either way.
// =============================================================================
public enum StreamMode
{
    Samples,
    Segments,
}

// =============================================================================
// SegmentSample — one 0x0085 segment handed from the MFP event thread to the
// connection task. Sentinel=true means "no end velocity" (encoded INT16_MIN).
// =============================================================================
public readonly struct SegmentSample
{
    public readonly double Target;     // normalized 0..1 (pre-wire)
    public readonly int DurationMs;    // commanded segment duration, ≥1
    public readonly double EndVel;     // norm/s (ignored when Sentinel)
    public readonly bool Sentinel;     // true → INT16_MIN "no end velocity"
    public SegmentSample(double target, int durationMs, double endVel, bool sentinel)
    {
        Target = target; DurationMs = durationMs; EndVel = endVel; Sentinel = sentinel;
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
    public const int TokenBytes = 16;                      // limits.token_bytes
    public const double ClockResyncIntervalMs = 10_000;    // limits.clock_resync_interval_s

    // ---- Frame types (registry frame_types) ---------------------------------
    // v1.0 note: 0x09 CATALOG_REQ and 0x0A CATALOG_CHUNK are RETIRED and their
    // numbers BURNED. Catalog transfer is now blob namespace 0 over the
    // generalized BLOB_REQ(0x1A)/BLOB_CHUNK(0x1B) pair (RFC-021).
    public const byte FHello = 0x00;
    public const byte FWelcome = 0x01;
    public const byte FPing = 0x03;
    public const byte FPong = 0x04;
    public const byte FClock = 0x05;
    public const byte FSubscribe = 0x06;
    public const byte FUnsubscribe = 0x07;
    public const byte FGrant = 0x08;
    public const byte FState = 0x0B;
    public const byte FStream = 0x0C;
    public const byte FIntent = 0x0D;
    public const byte FEcho = 0x0E;
    public const byte FEvent = 0x0F;
    public const byte FNack = 0x10;
    public const byte FGoodbye = 0x11;
    public const byte FProbe = 0x12;
    public const byte FProbeReport = 0x13;
    public const byte FPairReq = 0x14;
    public const byte FPairGrant = 0x15;
    public const byte FAckMask = 0x16;
    public const byte FBeacon = 0x17;
    public const byte FPublish = 0x18;        // §6.6 mid-session publish renegotiation
    public const byte FCatalogReady = 0x19;   // §8.4 readiness gate (raw: 8 etag bytes)
    public const byte FBlobReq = 0x1A;        // §8.4 / RFC-021 generalized transfer request
    public const byte FBlobChunk = 0x1B;      // §8.4 / RFC-021 chunk (raw, 14-byte header)
    public const byte FAuth = 0x1C;           // §12.2 post-WELCOME token proof
    public const byte FHubSig = 0x1D;         // §12.2 deferred WELCOME signature
    public const byte FEstop = 0xE5;

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
    public const int KIntentId = 18;
    public const int KApplied = 19;
    public const int KValue = 20;
    public const int KTimestamp = 21;
    public const int KLimits = 22;
    public const int KRoles = 23;
    public const int KDeadmanMs = 24;
    public const int KDeadmanPolicy = 25;
    public const int KChunks = 27;            // BLOB_REQ selective repair: missing chunk indices
    public const int KNonce = 29;
    public const int KEventKind = 33;
    public const int KGrants = 35;
    public const int KGrantedPublishes = 36;
    public const int KBlob = 38;              // BLOB_REQ/CHUNK identity sub-map (RFC-021)
    public const int KTrust = 39;             // HELLO/WELCOME/AUTH trust sub-map (RFC-029)
    public const int KBody = 40;              // EVENT: the kind-specific field sub-map
    public const int KIntentSeq = 41;         // NACK: header seq of the frame being refused (RFC-001)

    // ---- `blob` (38) sub-map keys (registry blob_keys, RFC-021) -------------
    // ONE vocabulary shared by BLOB_REQ's CBOR map and BLOB_CHUNK's fixed
    // binary header, so the two can never drift apart.
    public const int BlobKNs = 1;
    public const int BlobKStoreId = 2;
    public const int BlobKSlot = 3;
    public const int BlobKGeneration = 4;
    public const int BlobKChunkIndex = 8;
    public const int BlobKChunkCount = 9;
    public const int BlobKTotalBytes = 10;

    // ---- Blob namespaces (registry blob_namespaces) -------------------------
    public const int BlobNsCatalog = 0;   // the ONLY namespace with a READY concept
    public const int BlobNsStore = 1;

    // BLOB_CHUNK (0x1B) raw payload header — 14 bytes, little-endian:
    //   ns u8 | store_id u8 | slot u8 | reserved u8 | generation u16 |
    //   chunk_index u16 | chunk_count u16 | total_bytes u32
    // The reserved byte is IGNORED, never validated — that is what keeps it
    // usable later without a wire break.
    public const int BlobChunkHeaderBytes = 14;

    // ---- Transfer / readiness limits (registry limits) ----------------------
    public const int EtagBytes = 8;                        // limits.etag_bytes
    public const int CatalogChunkPayload = 192;            // limits.catalog_chunk_payload
    public const int CatalogChunkGapTimeoutMs = 500;       // limits.catalog_chunk_gap_timeout_ms
    public const int CatalogReadyTimeoutMs = 15_000;       // limits.catalog_ready_timeout_ms
    public const int CatalogMaxBytes = 262_144;            // RFC-028 sanity ceiling on a declared total

    // ---- Priorities (registry Priority) -------------------------------------
    public const byte PriorityBackground = 0;
    public const byte PriorityNormal = 1;
    public const byte PriorityElevated = 2;
    public const byte PriorityCritical = 3;

    // ---- Packed layout field types (registry PackedFieldType) ---------------
    // The catalog's `layout` describes a STATE payload as a flat little-endian
    // struct; these are the type tags and their wire sizes.
    public const int PackedU8 = 0, PackedI8 = 1, PackedU16 = 2, PackedI16 = 3;
    public const int PackedU32 = 4, PackedI32 = 5, PackedF32 = 6, PackedBitfield8 = 7;
    public const int PackedStr16 = 8, PackedStr32 = 9, PackedStr64 = 10;
    public static readonly int[] PackedSize = { 1, 1, 2, 2, 4, 4, 4, 1, 16, 32, 64 };

    // ---- Catalog CBOR keys (schema/catalog.cddl, Appendix C) ----------------
    public const int CatEId = 1, CatEName = 2, CatECls = 3, CatEDir = 4, CatEAccess = 5;
    public const int CatERate = 6, CatEPriority = 7, CatELayout = 8, CatESchema = 9;
    public const int CatECategory = 10, CatEStore = 12, CatESettingChannel = 14;
    public const int CatFName = 1, CatFType = 2, CatFUnit = 3, CatFScale = 4;
    public const int CatFMin = 5, CatFMax = 6, CatFBits = 7, CatFSettingKey = 8;
    public const int CatFDefault = 9, CatFOptions = 10, CatFGroup = 11, CatFDesc = 12;
    public const int CatFRole = 13, CatFStep = 14, CatFFlags = 15;

    // ---- Field roles (registry field_roles) ---------------------------------
    // RFC-006(b): the portable way to find a semantic field on ANY hub's
    // catalog without knowing its channel numbering. A role this client does
    // not recognize renders generically; a role the hub never declares is
    // simply absent, and the client says so rather than guessing.
    public const string RoleLimitInputSpeed = "limit.input.speed";
    public const string RoleLimitInputAccel = "limit.input.accel";
    public const string RoleLimitInputJerk = "limit.input.jerk";
    public const string RoleLimitUserSpeed = "limit.user.speed";
    public const string RoleLimitUserAccel = "limit.user.accel";
    public const string RoleWindowMin = "window.min";
    public const string RoleWindowMax = "window.max";

    // ---- Device channel ids (include/comms/SlopSyncCatalog.h) ---------------
    // NOTE the asymmetry, and it is deliberate: the PUBLISH channels below are
    // this client's compiled-in wire contract (it WRITES those layouts, §8.5
    // static profile). The channels it READS — limits, window — are NOT
    // hardcoded anywhere; they are located by field ROLE in the fetched
    // catalog (see SlopCatalog.LocateRole). 0x0081 appears nowhere in this file.
    public const ushort ChSafety = 0x0003;        // channels.safety (STATE, critical)
    public const ushort ChMotion = 0x0080;        // ch::motion (STATE)
    public const ushort ChMotionInput = 0x0084;   // ch::motion_input (STREAM c2h, ≤333 Hz)
    public const ushort ChMotionSegment = 0x0085; // ch::motion_segment (STREAM c2h, ≤50 Hz, timed segments)
    public const ushort ChHome = 0x0103;          // ch::home (INTENT, control)

    // 0x0103 `op` (registry home ops). 1 = home; 2 force_home CLEARS the e-stop
    // latch and 3 clears the override — neither is a thing a media player
    // should be able to do, so only `home` is exposed.
    public const int HomeOpHome = 1;

    // §0x0085 sentinel: "no end velocity" (INT16_MIN). 0 is a real slope.
    public const short SegmentEndVelSentinel = short.MinValue;   // -32768

    // ---- NACK codes (registry NackCode) -------------------------------------
    // Named constants, not literals: GOODBYE draws its code from this same
    // vocabulary (§6.8), so a magic 0x0107 in a send path is a number with no
    // name attached to it.
    public const ushort NackMalformed = 0x0000;
    public const ushort NackUnsupportedVersion = 0x0001;
    public const ushort NackFrameTooLarge = 0x0002;
    public const ushort NackProfileViolation = 0x0003;
    public const ushort NackBusy = 0x0100;
    public const ushort NackUnauthorized = 0x0101;
    public const ushort NackNotController = 0x0102;
    public const ushort NackPairingRequired = 0x0103;
    public const ushort NackPairingDenied = 0x0104;
    public const ushort NackSessionEvicted = 0x0105;
    public const ushort NackDuplicateInstance = 0x0106;
    public const ushort NackNormalClosure = 0x0107;
    public const ushort NackDeadmanTimeout = 0x0108;
    public const ushort NackReadyTimeout = 0x010A;    // never sent CATALOG_READY in time (GOODBYE code)
    public const ushort NackNotReady = 0x010B;        // frame refused: session has not declared readiness
    public const ushort NackUnknownChannel = 0x0200;
    public const ushort NackAccessDenied = 0x0201;
    public const ushort NackClassMismatch = 0x0202;
    public const ushort NackSubLimit = 0x0203;
    public const ushort NackConflict = 0x0300;
    public const ushort NackRateLimited = 0x0301;
    public const ushort NackInvalidValue = 0x0302;
    public const ushort NackUnsupportedOp = 0x0303;
    public const ushort NackEstopActive = 0x0400;
    public const ushort NackNotHomed = 0x0401;
    public const ushort NackInterlock = 0x0402;
    public const ushort NackSourceConflict = 0x0403;
    public const ushort NackTakeoverRequired = 0x0404;
    public const ushort NackClearRefused = 0x0405;
    public const ushort NackChunkUnavailable = 0x0500;
    public const ushort NackReassemblyTimeout = 0x0501;
    public const ushort NackEtagMismatch = 0x0502;

    // §6.8: GOODBYE's `code` is drawn from the NackCode vocabulary. This is an
    // ALIAS of NORMAL_CLOSURE, not a second number.
    public const ushort GoodbyeNormalClosure = NackNormalClosure;

    public static readonly IReadOnlyDictionary<ushort, string> NackNames = new Dictionary<ushort, string>
    {
        [NackMalformed] = "MALFORMED", [NackUnsupportedVersion] = "UNSUPPORTED_VERSION",
        [NackFrameTooLarge] = "FRAME_TOO_LARGE", [NackProfileViolation] = "PROFILE_VIOLATION",
        [NackBusy] = "BUSY", [NackUnauthorized] = "UNAUTHORIZED",
        [NackNotController] = "NOT_CONTROLLER", [NackPairingRequired] = "PAIRING_REQUIRED",
        [NackPairingDenied] = "PAIRING_DENIED", [NackSessionEvicted] = "SESSION_EVICTED",
        [NackDuplicateInstance] = "DUPLICATE_INSTANCE", [NackNormalClosure] = "NORMAL_CLOSURE",
        [NackDeadmanTimeout] = "DEADMAN_TIMEOUT", [NackReadyTimeout] = "READY_TIMEOUT",
        [NackNotReady] = "NOT_READY", [NackUnknownChannel] = "UNKNOWN_CHANNEL",
        [NackAccessDenied] = "ACCESS_DENIED", [NackClassMismatch] = "CLASS_MISMATCH",
        [NackSubLimit] = "SUB_LIMIT", [NackConflict] = "CONFLICT",
        [NackRateLimited] = "RATE_LIMITED", [NackInvalidValue] = "INVALID_VALUE",
        [NackUnsupportedOp] = "UNSUPPORTED_OP", [NackEstopActive] = "ESTOP_ACTIVE",
        [NackNotHomed] = "NOT_HOMED", [NackInterlock] = "INTERLOCK",
        [NackSourceConflict] = "SOURCE_CONFLICT", [NackTakeoverRequired] = "TAKEOVER_REQUIRED",
        [NackClearRefused] = "CLEAR_REFUSED", [NackChunkUnavailable] = "CHUNK_UNAVAILABLE",
        [NackReassemblyTimeout] = "REASSEMBLY_TIMEOUT", [NackEtagMismatch] = "ETAG_MISMATCH",
    };

    public static string NackName(ushort code) =>
        NackNames.TryGetValue(code, out var n) ? n : $"0x{code:X4}";

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

    // ---- HELLO (§6.2) — keys ascending: 1<2<3<4<(5)<(8)<(10)<11 -------------
    // publishes wish:     [{12:rate_hz, 15:channel_id}]            (c2h STREAM)
    // subscriptions wish: [{12:rate_hz, 13:priority, 15:channel_id}]
    // catalog_etag (8):   RFC-015's fast path — presenting an etag the hub
    //                     agrees with makes the session READY at WELCOME, with
    //                     no BLOB transfer and no CATALOG_READY frame at all.
    //
    // §6.2 exists so a simple client can complete setup in ONE round trip;
    // this client now uses that for its read channels too (RFC-006: a motion
    // producer that subscribes to nothing is flying blind). The channel whose
    // fields carry the kinematic ROLES cannot go here — it is only knowable
    // once the catalog is decoded — so that one is a mid-session SUBSCRIBE.
    public static byte[] BuildHello(string clientKind, string clientName, byte[] instanceId,
                                    ushort publishChannel, double publishRateHz, byte[] token16 = null)
        => BuildHello(clientKind, clientName, instanceId,
                      new (ushort ch, double rate)[] { (publishChannel, publishRateHz) }, token16);

    // Multi-wish variant: the publishes array carries one {12:rate,15:channel}
    // map per channel we want to publish on (§6.2). Segments mode wishes both
    // 0x0084 and 0x0085; Samples mode wishes just 0x0084 (the single-wish overload
    // above delegates here, producing byte-identical output for one channel).
    public static byte[] BuildHello(string clientKind, string clientName, byte[] instanceId,
                                    IReadOnlyList<(ushort ch, double rate)> publishes, byte[] token16 = null,
                                    IReadOnlyList<(ushort ch, double rate, byte prio)> subscribes = null,
                                    byte[] catalogEtag = null)
    {
        bool hasSubs = subscribes != null && subscribes.Count > 0;
        bool hasEtag = catalogEtag != null && catalogEtag.Length > 0;

        var w = new CborWriter();
        int n = 4 + (token16 != null ? 1 : 0) + (hasEtag ? 1 : 0) + (hasSubs ? 1 : 0) + 1;
        w.WriteMapHeader(n);
        w.WriteUInt(KProtoVer); w.WriteUInt(ProtocolVersion);
        w.WriteUInt(KClientKind); w.WriteTextString(clientKind);
        w.WriteUInt(KClientName); w.WriteTextString(clientName);
        w.WriteUInt(KInstanceId); w.WriteByteString(instanceId);
        if (token16 != null) { w.WriteUInt(KToken); w.WriteByteString(token16); }
        if (hasEtag) { w.WriteUInt(KCatalogEtag); w.WriteByteString(catalogEtag); }
        if (hasSubs)
        {
            w.WriteUInt(KSubscriptions);
            w.WriteArrayHeader(subscribes.Count);
            foreach (var (ch, rate, prio) in subscribes)
            {
                w.WriteMapHeader(3);                  // {12:rate, 13:prio, 15:channel} ascending
                w.WriteUInt(KRateHz); w.WriteFloat32((float)rate);
                w.WriteUInt(KPriority); w.WriteUInt(prio);
                w.WriteUInt(KChannelId); w.WriteUInt(ch);
            }
        }
        w.WriteUInt(KPublishes);
        w.WriteArrayHeader(publishes.Count);
        foreach (var (ch, rate) in publishes)
        {
            w.WriteMapHeader(2);                      // {12:rate, 15:channel} keys ascending
            w.WriteUInt(KRateHz); w.WriteFloat32((float)rate);
            w.WriteUInt(KChannelId); w.WriteUInt(ch);
        }
        return w.ToArray();
    }

    // ---- BLOB_REQ (0x1A, §8.4 / RFC-021) ------------------------------------
    // A bare catalog request is the EMPTY CBOR map (0xA0): namespace 0 is the
    // default and store_id/slot are absent by rule, so generalizing transfer
    // cost the common case exactly zero bytes. The `blob` (38) sub-map is
    // emitted ONLY when it says something a default does not. Key order
    // ascending: chunks(27) before blob(38).
    public static byte[] BuildBlobReq(int ns = BlobNsCatalog, int? storeId = null, int? slot = null,
                                      int? generation = null, IReadOnlyList<int> chunks = null)
    {
        bool hasChunks = chunks != null && chunks.Count > 0;
        var sub = new List<(int key, long val)>();
        if (ns != BlobNsCatalog) sub.Add((BlobKNs, ns));
        if (storeId.HasValue) sub.Add((BlobKStoreId, storeId.Value));
        if (slot.HasValue) sub.Add((BlobKSlot, slot.Value));
        if (generation.HasValue) sub.Add((BlobKGeneration, generation.Value));

        var w = new CborWriter();
        w.WriteMapHeader((hasChunks ? 1 : 0) + (sub.Count > 0 ? 1 : 0));
        if (hasChunks)
        {
            w.WriteUInt(KChunks);
            w.WriteArrayHeader(chunks.Count);
            foreach (var i in chunks) w.WriteUInt(i);
        }
        if (sub.Count > 0)
        {
            w.WriteUInt(KBlob);
            w.WriteMapHeader(sub.Count);
            foreach (var (k, v) in sub) { w.WriteUInt(k); w.WriteUInt(v); }
        }
        return w.ToArray();
    }

    /// <summary>Full transfer of blob namespace 0 (the catalog): the empty map.</summary>
    public static byte[] BuildCatalogRequest() => BuildBlobReq();

    /// <summary>Selective repair of the catalog: {27:[indices]}. A repair naming
    /// ZERO chunks is MALFORMED (RFC-022.6) — callers must not send one.</summary>
    public static byte[] BuildCatalogRepair(IReadOnlyList<int> indices) => BuildBlobReq(chunks: indices);

    public readonly struct BlobChunk
    {
        public readonly byte Ns, StoreId, Slot;
        public readonly ushort Generation, ChunkIndex, ChunkCount;
        public readonly uint TotalBytes;
        public readonly byte[] Bytes;
        public BlobChunk(byte ns, byte store, byte slot, ushort gen, ushort idx, ushort count,
                         uint total, byte[] bytes)
        { Ns = ns; StoreId = store; Slot = slot; Generation = gen; ChunkIndex = idx; ChunkCount = count; TotalBytes = total; Bytes = bytes; }
    }

    /// <summary>Parse a BLOB_CHUNK (0x1B) raw payload's 14-byte identity header.
    /// The reserved byte is IGNORED, never validated.</summary>
    public static bool TryDecodeBlobChunk(byte[] payload, out BlobChunk chunk)
    {
        chunk = default;
        if (payload == null || payload.Length < BlobChunkHeaderBytes) return false;
        var s = payload.AsSpan();
        var body = new byte[payload.Length - BlobChunkHeaderBytes];
        Array.Copy(payload, BlobChunkHeaderBytes, body, 0, body.Length);
        chunk = new BlobChunk(
            s[0], s[1], s[2],
            BinaryPrimitives.ReadUInt16LittleEndian(s.Slice(4)),
            BinaryPrimitives.ReadUInt16LittleEndian(s.Slice(6)),
            BinaryPrimitives.ReadUInt16LittleEndian(s.Slice(8)),
            BinaryPrimitives.ReadUInt32LittleEndian(s.Slice(10)),
            body);
        return true;
    }

    // ---- INTENT (§9.3) — {15:channel, 18:intent_id, 20:{value map}} ---------
    // `value`'s integer sub-keys come from the CHANNEL'S CATALOG schema, never
    // from the global key space. Values are pre-encoded by the caller because
    // the schema decides the CBOR type (f32_t vs uint_t vs bool_t) per key.
    public static byte[] BuildIntent(ushort channelId, long intentId,
                                     IReadOnlyList<(int key, byte[] encoded)> valueFields)
    {
        var w = new CborWriter();
        w.WriteMapHeader(3);                          // keys ascending 15<18<20
        w.WriteUInt(KChannelId); w.WriteUInt(channelId);
        w.WriteUInt(KIntentId); w.WriteUInt(intentId);
        w.WriteUInt(KValue);
        w.WriteMapHeader(valueFields.Count);
        foreach (var (key, enc) in valueFields) { w.WriteUInt(key); w.WriteRaw(enc); }
        return w.ToArray();
    }

    /// <summary>One pre-encoded f32 CBOR value, for an INTENT `value` entry.</summary>
    public static byte[] CborF32(double v) { var w = new CborWriter(); w.WriteFloat32((float)v); return w.ToArray(); }
    /// <summary>One pre-encoded unsigned CBOR value, for an INTENT `value` entry.</summary>
    public static byte[] CborUInt(long v) { var w = new CborWriter(); w.WriteUInt(v); return w.ToArray(); }

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

    // ---- SEGMENT bundle (§5.4) for motion-segment (0x0085) ------------------
    // Same STREAM framing as 0x0084: [t_base:u32 LE][n:u8][reserved:u8][off:u16 LE]*n
    // then n × 6-byte samples {target:u16 LE, duration:u16 LE, end_vel:i16 LE}.
    // Scales (SlopSyncCatalog 0x0085): target *10000, duration *1, end_vel *1000.
    // duration is clamped ≥1 (the channel rejects 0); a Sentinel end_vel encodes
    // INT16_MIN, and a real end_vel is clamped to ±32767 so it can never collide
    // with the sentinel.
    public static byte[] BuildSegmentBundle(uint tBase, IReadOnlyList<(ushort off, double target, int durationMs, double endVel, bool sentinel)> samples)
    {
        int n = samples.Count;
        var buf = new byte[6 + n * 2 + n * 6];
        int p = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(p), tBase); p += 4;
        buf[p++] = (byte)n;
        buf[p++] = 0;                                  // reserved
        for (int i = 0; i < n; i++) { BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), samples[i].off); p += 2; }
        for (int i = 0; i < n; i++)
        {
            int rawT = Math.Clamp((int)Math.Round(samples[i].target * 10000.0), 0, 65535);
            int rawD = Math.Clamp(samples[i].durationMs, 1, 65535);
            short rawV = samples[i].sentinel
                ? SegmentEndVelSentinel
                : (short)Math.Clamp((int)Math.Round(samples[i].endVel * 1000.0), -32767, 32767);
            BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), (ushort)rawT); p += 2;
            BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), (ushort)rawD); p += 2;
            BinaryPrimitives.WriteInt16LittleEndian(buf.AsSpan(p), rawV); p += 2;
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

    /// <summary>Splice already-encoded CBOR in verbatim. Used for INTENT `value`
    /// entries, whose CBOR type is decided per-key by the channel's catalog
    /// schema rather than by this writer.</summary>
    public void WriteRaw(ReadOnlySpan<byte> encoded) => _ms.Write(encoded);

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
// SlopCatalog — the hub's self-describing channel list (§8.1), decoded from the
// blob-namespace-0 bytes, plus the RFC-006(b) ROLE LOOKUP that is the whole
// point of fetching it here.
//
// WHY THIS CLIENT FETCHES A CATALOG AT ALL (it never did before):
// This plugin's PUBLISH layouts (0x0084 / 0x0085) are and stay compiled in —
// they are the wire contract it WRITES, a §8.5 static profile, and no amount
// of catalog would change what a funscript sample looks like. But the thing it
// now READS — "what are this machine's input speed / accel / jerk ceilings and
// where is its stroke window?" — has NO portable channel number. On this device
// they live on 0x0081, but 0x0081 is in the DEVICE'S OWN >=0x0080 allocation,
// not the registry's reserved range: another conforming hub may put them
// anywhere, or not have them. Hardcoding 0x0081 is exactly the coupling the
// self-describing catalog exists to prevent (RFC-006 gap 2). So the fields are
// located by their registry `field_roles` instead, which works on any hub that
// annotates its catalog and degrades to an honest "not advertised" on one that
// does not.
//
// THE CATALOG IS ALSO THE DECODER RING. A STATE payload is a flat packed
// struct with no self-description; the only way to know that "the 5th f32 is
// input_speed in mm/s" is the layout the hub itself published. There is
// deliberately NO fallback layout table here — a hand-copied one is a lie
// waiting to happen (the WebUI had exactly that bug), and RFC-015's readiness
// gate means a STATE frame can no longer arrive before its decoder ring does.
// =============================================================================
public sealed class SlopCatalog
{
    /// <summary>One packed-layout field, with its byte offset precomputed.</summary>
    public sealed class Field
    {
        public string Name;
        public int Type = SlopWire.PackedU8;
        public double Scale = 1.0;
        public string Unit;
        public string Role;          // registry field_roles value, or null
        public int Offset;           // byte offset into the STATE payload
        public int Size;             // wire size in bytes
        public int? SettingKey;      // paired INTENT key, or null = READ-ONLY (RFC-003)
        public double? Min, Max, Step;
        public string Group, Desc;
    }

    public sealed class Entry
    {
        public ushort Id;
        public string Name;
        public int Cls;                       // ChannelClass
        public ushort? SettingChannel;        // paired INTENT channel (catalog key 14)
        public readonly List<Field> Layout = new();
        public readonly Dictionary<int, int> SchemaTypes = new();   // key -> CborFieldType
    }

    /// <summary>Everything needed to READ a role's live value and (if writable)
    /// WRITE it back — resolved once, at catalog adoption.</summary>
    public sealed class RoleLocator
    {
        public string Role;
        public ushort ChannelId;              // the STATE channel carrying it
        public Field Field;
        public ushort? SettingChannel;        // paired INTENT channel
        public int? SettingKey;               // key within that INTENT's `value` map
        public bool Writable => SettingChannel.HasValue && SettingKey.HasValue;
    }

    private readonly Dictionary<ushort, Entry> _entries = new();
    private readonly Dictionary<string, RoleLocator> _roles = new(StringComparer.Ordinal);

    public IReadOnlyDictionary<ushort, Entry> Entries => _entries;
    public int RoleCount => _roles.Count;

    public Entry Channel(ushort id) => _entries.TryGetValue(id, out var e) ? e : null;

    /// <summary>Locate a registry field-role on THIS hub, or null when the hub
    /// does not advertise it. Null is a first-class answer — the caller shows
    /// "not advertised", never a guessed channel number.</summary>
    public RoleLocator LocateRole(string role) => _roles.TryGetValue(role, out var r) ? r : null;

    /// <summary>Decode a catalog blob (array of channel entries). Returns null
    /// on anything undecodable — the caller treats that as "no decoder ring",
    /// never as "the hub is wrong".</summary>
    public static SlopCatalog Decode(byte[] bytes)
    {
        if (bytes == null || bytes.Length == 0) return null;
        List<object> arr;
        try { arr = new CborReader(bytes).Decode() as List<object>; }
        catch (Exception) { return null; }
        if (arr == null) return null;

        var cat = new SlopCatalog();
        foreach (var item in arr)
        {
            if (item is not Dictionary<long, object> m) continue;
            if (!m.TryGetValue(SlopWire.CatEId, out var idv)) continue;

            var e = new Entry { Id = (ushort)Convert.ToInt64(idv) };
            if (m.TryGetValue(SlopWire.CatEName, out var nm)) e.Name = nm as string;
            if (m.TryGetValue(SlopWire.CatECls, out var cl)) e.Cls = (int)Convert.ToInt64(cl);
            if (m.TryGetValue(SlopWire.CatESettingChannel, out var sc))
                e.SettingChannel = (ushort)Convert.ToInt64(sc);

            // Packed layout: offsets accumulate in declaration order. An UNKNOWN
            // packed type makes every later offset unknowable, so we stop there
            // rather than silently mis-decoding the tail.
            if (m.TryGetValue(SlopWire.CatELayout, out var lay) && lay is List<object> fields)
            {
                int off = 0;
                foreach (var fo in fields)
                {
                    if (fo is not Dictionary<long, object> fm) continue;
                    var f = new Field { Offset = off };
                    if (fm.TryGetValue(SlopWire.CatFName, out var fn)) f.Name = fn as string;
                    if (fm.TryGetValue(SlopWire.CatFType, out var ft)) f.Type = (int)Convert.ToInt64(ft);
                    if (fm.TryGetValue(SlopWire.CatFUnit, out var fu)) f.Unit = fu as string;
                    if (fm.TryGetValue(SlopWire.CatFScale, out var fs)) f.Scale = Convert.ToDouble(fs);
                    if (fm.TryGetValue(SlopWire.CatFRole, out var fr)) f.Role = fr as string;
                    if (fm.TryGetValue(SlopWire.CatFSettingKey, out var sk)) f.SettingKey = (int)Convert.ToInt64(sk);
                    if (fm.TryGetValue(SlopWire.CatFMin, out var mn)) f.Min = Convert.ToDouble(mn);
                    if (fm.TryGetValue(SlopWire.CatFMax, out var mx)) f.Max = Convert.ToDouble(mx);
                    if (fm.TryGetValue(SlopWire.CatFStep, out var st)) f.Step = Convert.ToDouble(st);
                    if (fm.TryGetValue(SlopWire.CatFGroup, out var gp)) f.Group = gp as string;
                    if (fm.TryGetValue(SlopWire.CatFDesc, out var ds)) f.Desc = ds as string;
                    if (f.Scale == 0 || double.IsNaN(f.Scale)) f.Scale = 1.0;

                    if (f.Type < 0 || f.Type >= SlopWire.PackedSize.Length) break;
                    f.Size = SlopWire.PackedSize[f.Type];
                    off += f.Size;
                    e.Layout.Add(f);
                }
            }

            // INTENT schema: key -> CborFieldType, so a writer knows whether a
            // key wants f32 or uint without hardcoding it.
            if (m.TryGetValue(SlopWire.CatESchema, out var sch) && sch is Dictionary<long, object> sm)
            {
                foreach (var kv in sm)
                {
                    if (kv.Value is Dictionary<long, object> sf &&
                        sf.TryGetValue(SlopWire.CatFType, out var ty))
                        e.SchemaTypes[(int)kv.Key] = (int)Convert.ToInt64(ty);
                }
            }

            cat._entries[e.Id] = e;
        }

        // Second pass: index every declared role. First declaration wins — a
        // hub declaring the same role twice is ambiguous, and picking the first
        // is at least deterministic.
        foreach (var e in cat._entries.Values)
        {
            foreach (var f in e.Layout)
            {
                if (string.IsNullOrEmpty(f.Role) || cat._roles.ContainsKey(f.Role)) continue;
                cat._roles[f.Role] = new RoleLocator
                {
                    Role = f.Role,
                    ChannelId = e.Id,
                    Field = f,
                    SettingChannel = f.SettingKey.HasValue ? e.SettingChannel : null,
                    SettingKey = f.SettingKey,
                };
            }
        }
        return cat;
    }

    /// <summary>Read one packed field out of a STATE payload. Returns NaN when
    /// the frame is shorter than the layout says (an older firmware talking to
    /// a newer catalog, or a truncated frame) — never a garbage number.</summary>
    public static double ReadField(byte[] payload, Field f)
    {
        if (payload == null || f == null) return double.NaN;
        if (f.Offset + f.Size > payload.Length) return double.NaN;
        var s = payload.AsSpan(f.Offset);
        double raw;
        switch (f.Type)
        {
            case SlopWire.PackedU8:
            case SlopWire.PackedBitfield8: raw = s[0]; break;
            case SlopWire.PackedI8: raw = (sbyte)s[0]; break;
            case SlopWire.PackedU16: raw = BinaryPrimitives.ReadUInt16LittleEndian(s); break;
            case SlopWire.PackedI16: raw = BinaryPrimitives.ReadInt16LittleEndian(s); break;
            case SlopWire.PackedU32: raw = BinaryPrimitives.ReadUInt32LittleEndian(s); break;
            case SlopWire.PackedI32: raw = BinaryPrimitives.ReadInt32LittleEndian(s); break;
            case SlopWire.PackedF32: raw = BinaryPrimitives.ReadSingleLittleEndian(s); break;
            default: return double.NaN;      // strings have no numeric reading
        }
        // A bitfield8 is a raw byte; scale is meaningless on it.
        if (f.Type == SlopWire.PackedBitfield8) return raw;
        return f.Scale != 1.0 ? raw / f.Scale : raw;
    }

    /// <summary>SHA-256 of the catalog bytes, truncated to `etag_bytes` (8) —
    /// the etag the hub advertises and the one CATALOG_READY declares.</summary>
    public static byte[] Etag(byte[] bytes)
    {
        using var sha = System.Security.Cryptography.SHA256.Create();
        var d = sha.ComputeHash(bytes);
        var e = new byte[SlopWire.EtagBytes];
        Array.Copy(d, e, SlopWire.EtagBytes);
        return e;
    }

    public static bool BytesEqual(byte[] a, byte[] b)
    {
        if (a == null || b == null || a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }

    public static string Hex(byte[] b) => b == null ? "<none>" : Convert.ToHexString(b).ToLowerInvariant();
}

// =============================================================================
// BlobReassembler — chunk collection for one BLOB transfer (§8.4 / RFC-021).
// Bounded by the declared total_bytes (RFC-028: never allocate on an unbounded
// declaration) and able to name its own gaps for a selective repair.
// =============================================================================
public sealed class BlobReassembler
{
    private readonly Dictionary<int, byte[]> _chunks = new();
    public int ChunkCount { get; private set; } = -1;
    public uint TotalBytes { get; private set; }
    public bool Active { get; private set; }

    public void Reset() { _chunks.Clear(); ChunkCount = -1; TotalBytes = 0; Active = false; }

    /// <summary>Insert a chunk. Returns false if the declared size is refused or
    /// the chunk belongs to a different transfer than the one in progress.</summary>
    public bool Insert(in SlopWire.BlobChunk c)
    {
        if (c.TotalBytes > SlopWire.CatalogMaxBytes || c.ChunkCount == 0) return false;
        if (!Active || ChunkCount != c.ChunkCount || TotalBytes != c.TotalBytes)
        {
            _chunks.Clear();
            ChunkCount = c.ChunkCount;
            TotalBytes = c.TotalBytes;
            Active = true;
        }
        if (c.ChunkIndex >= ChunkCount) return false;
        _chunks[c.ChunkIndex] = c.Bytes;
        return true;
    }

    public bool Complete => Active && _chunks.Count == ChunkCount;

    public List<int> MissingIndices()
    {
        var missing = new List<int>();
        if (!Active) return missing;
        for (int i = 0; i < ChunkCount; i++) if (!_chunks.ContainsKey(i)) missing.Add(i);
        return missing;
    }

    /// <summary>Concatenate in index order. Null if the assembled length does not
    /// match the header's declared total_bytes.</summary>
    public byte[] Assemble()
    {
        if (!Complete) return null;
        int len = 0;
        for (int i = 0; i < ChunkCount; i++) len += _chunks[i].Length;
        if (len != TotalBytes) return null;
        var outBuf = new byte[len];
        int p = 0;
        for (int i = 0; i < ChunkCount; i++) { var c = _chunks[i]; Array.Copy(c, 0, outBuf, p, c.Length); p += c.Length; }
        return outBuf;
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
    private ushort _segmentSeq;          // per-channel seq for 0x0085 (§7.3)

    // CLOCK reply plumbing: the receive loop captures t3 at arrival and hands
    // the raw (t0e,t1,t2,t3) to whoever is awaiting an exchange.
    private TaskCompletionSource<(uint t0e, uint t1, uint t2, uint t3)> _pendingClock;

    private Action<NackInfo> _onNack;
    private Action<ushort, byte[]> _onState;
    private Action<EchoInfo> _onEcho;

    /// <summary>A refused frame (§16.1). `IntentSeq` is RFC-001's correlation
    /// key, stamped by the hub from the refused frame's HEADER seq.</summary>
    public sealed class NackInfo
    {
        public ushort Code;
        public ushort Channel;
        public string Detail;
        public long? IntentSeq;
        public string Name => SlopWire.NackName(Code);
    }

    /// <summary>An applied intent (§9.3). `Applied` carries the POST-CLAMP
    /// values — the only ones a client may display.</summary>
    public sealed class EchoInfo
    {
        public ushort Channel;
        public long IntentId;
        public long CfgGen;
        public Dictionary<long, object> Applied;

        public bool TryGetApplied(int key, out double value)
        {
            value = double.NaN;
            if (Applied == null || !Applied.TryGetValue(key, out var v) || v == null) return false;
            try { value = Convert.ToDouble(v); return !double.IsNaN(value); }
            catch { return false; }
        }
    }

    public HubClient(ClientWebSocket ws, byte[] instanceId, Logger log)
    {
        _ws = ws; _instanceId = instanceId; _log = log;
    }

    /// <summary>Attach/replace the ECHO handler after the receive loop is
    /// already running (used by the LiveWireTest harness).</summary>
    public void SetEchoHandler(Action<EchoInfo> onEcho) => _onEcho = onEcho;

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
    public Task<WelcomeInfo> HelloAsync(string kind, string name, ushort publishChannel,
        double publishRateHz, byte[] token16, CancellationToken token)
        => HelloAsync(kind, name,
                      new (ushort ch, double rate)[] { (publishChannel, publishRateHz) }, token16, token);

    public async Task<WelcomeInfo> HelloAsync(string kind, string name,
        IReadOnlyList<(ushort ch, double rate)> publishes, byte[] token16, CancellationToken token,
        IReadOnlyList<(ushort ch, double rate, byte prio)> subscribes = null,
        byte[] cachedEtag = null)
    {
        var hello = SlopWire.BuildHello(kind, name, _instanceId, publishes, token16, subscribes, cachedEtag);
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
                throw new InvalidOperationException($"HELLO refused: NACK {SlopWire.NackName(code)}");
            }
            // ignore anything else during the handshake (§4.3 tolerance)
        }
    }

    public async Task SubscribeAsync(IEnumerable<(ushort, double, byte)> wishes, CancellationToken token)
    {
        var payload = SlopWire.BuildSubscribe(wishes);
        await SendFrameAsync(SlopWire.FSubscribe, 0, payload, 0, token);
    }

    // =========================================================================
    // §8.4 / RFC-015 READINESS GATE
    //
    // Until a session declares WHICH catalog it decodes against, the hub emits
    // NO data-plane frame to it (no retained STATE, no STREAM) and answers
    // every INTENT with NOT_READY. That is not bureaucracy: a client that has
    // not adopted the retained safety latch must not be able to act (§11.5(2)).
    // A session that never declares readiness is GOODBYE'd with READY_TIMEOUT
    // after catalog_ready_timeout_ms.
    //
    // Two paths, and the fast one is the common one:
    //   * HELLO carried an etag the hub AGREES with  -> already ready at
    //     WELCOME. Zero extra frames, zero transfer. This is every reconnect
    //     after the first.
    //   * otherwise -> BLOB_REQ, reassemble BLOB_CHUNKs, verify the SHA-256
    //     LOCALLY, then declare CATALOG_READY carrying the etag we just proved
    //     we hold.
    //
    // The honest-degraded case matters: if the transfer does NOT verify against
    // the hub's advertised etag we declare the digest of the bytes we ACTUALLY
    // hold, so the hub can flag the mismatch rather than be misled by us
    // claiming an etag we cannot back up.
    // =========================================================================

    /// <summary>Declare readiness. Raw frame, payload = the 8 etag bytes.</summary>
    public Task SendCatalogReadyAsync(byte[] etag, CancellationToken token)
        => SendFrameAsync(SlopWire.FCatalogReady, 0, etag, 0, token);

    /// <summary>Fetch blob namespace 0 (the catalog) and reassemble it, with the
    /// §8.4 selective-repair cadence on a chunk gap. Returns the assembled bytes
    /// or null on timeout. Runs INLINE, before the receive loop starts.</summary>
    public async Task<byte[]> FetchCatalogAsync(CancellationToken token)
    {
        var blob = new BlobReassembler();
        await SendFrameAsync(SlopWire.FBlobReq, 0, SlopWire.BuildCatalogRequest(), 0, token);

        var sw = Stopwatch.StartNew();
        double lastChunkMs = 0;
        double lastRepairMs = 0;
        int repairs = 0;

        using var cts = CancellationTokenSource.CreateLinkedTokenSource(token);
        cts.CancelAfter(TimeSpan.FromMilliseconds(SlopWire.CatalogReadyTimeoutMs));

        while (!cts.IsCancellationRequested)
        {
            // Repair cadence: one BLOB_REQ naming the gaps per gap interval. A
            // repair naming zero chunks is MALFORMED, so `missing` is checked.
            double nowMs = sw.Elapsed.TotalMilliseconds;
            if (blob.Active && !blob.Complete &&
                nowMs - lastChunkMs >= SlopWire.CatalogChunkGapTimeoutMs &&
                nowMs - lastRepairMs >= SlopWire.CatalogChunkGapTimeoutMs)
            {
                var missing = blob.MissingIndices();
                if (missing.Count > 0 && repairs < 8)
                {
                    if (missing.Count > 32) missing = missing.GetRange(0, 32);
                    await SendFrameAsync(SlopWire.FBlobReq, 0, SlopWire.BuildCatalogRepair(missing), 0, token);
                    _log.Debug("BLOB repair: {0} missing chunk(s)", missing.Count);
                    lastRepairMs = nowMs;
                    repairs++;
                }
            }

            SlopWire.FrameHeader hdr;
            byte[] payload;
            try { (hdr, payload) = await ReceiveFrameAsync(cts.Token); }
            catch (OperationCanceledException) { break; }

            if (hdr.Type != SlopWire.FBlobChunk)
            {
                // The hub should not be sending us data-plane frames yet (we are
                // not READY), but §4.3 says tolerate whatever arrives.
                if (hdr.Type == SlopWire.FNack)
                {
                    var m = (Dictionary<long, object>)new CborReader(payload).Decode();
                    ushort code = m.TryGetValue(SlopWire.KCode, out var c) ? (ushort)Convert.ToInt64(c) : (ushort)0;
                    _log.Warn("catalog transfer NACK {0}", SlopWire.NackName(code));
                    if (code == SlopWire.NackChunkUnavailable) return null;
                }
                continue;
            }

            if (!SlopWire.TryDecodeBlobChunk(payload, out var chunk))
            {
                _log.Warn("BLOB_CHUNK shorter than the {0}-byte identity header", SlopWire.BlobChunkHeaderBytes);
                continue;
            }
            if (chunk.Ns != SlopWire.BlobNsCatalog) continue;   // not our namespace
            if (!blob.Insert(chunk))
            {
                _log.Warn("BLOB_CHUNK refused (declared total {0} B)", chunk.TotalBytes);
                continue;
            }
            lastChunkMs = sw.Elapsed.TotalMilliseconds;
            if (blob.Complete) break;
        }

        var bytes = blob.Assemble();
        if (bytes == null)
            _log.Warn("catalog transfer incomplete ({0}/{1} chunks)",
                      blob.ChunkCount - blob.MissingIndices().Count, blob.ChunkCount);
        return bytes;
    }

    // ---- INTENT (§9.3) ------------------------------------------------------
    // THE HEADER SEQ IS THE INTENT ID, deliberately. The hub stamps a NACK's
    // `intent_seq` (41) from the INBOUND FRAME HEADER's seq, not from the
    // decoded intent_id — so a client that sends header seq 0 on every intent
    // gets a correlation key that always reads 0 and correlates nothing. Making
    // the two the same number is what turns RFC-001's key into a usable one:
    // an ECHO names the intent_id, a NACK names the header seq, and they match.
    public Task SendIntentAsync(ushort channelId, long intentId,
                                IReadOnlyList<(int key, byte[] encoded)> value, CancellationToken token)
    {
        var payload = SlopWire.BuildIntent(channelId, intentId, value);
        return SendFrameAsync(SlopWire.FIntent, channelId, payload, (ushort)(intentId & 0xFFFF), token);
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

    // ---- SEGMENT sample (§0x0085) — single-sample bundle, own seq counter ---
    public Task SendSegmentSampleAsync(uint tBase, SegmentSample s, CancellationToken token)
    {
        var payload = SlopWire.BuildSegmentBundle(tBase,
            new (ushort, double, int, double, bool)[] { (0, s.Target, s.DurationMs, s.EndVel, s.Sentinel) });
        ushort seq = _segmentSeq++;
        return SendFrameAsync(SlopWire.FStream, SlopWire.ChMotionSegment, payload, seq, token);
    }

    // ---- PING keepalive (§6.5) — raw, empty payload; hub answers with PONG --
    public Task SendPingAsync(CancellationToken token)
        => SendFrameAsync(SlopWire.FPing, 0, Array.Empty<byte>(), 0, token);

    // ---- Receive loop -------------------------------------------------------
    // Single reader. Routes CLOCK replies to the pending exchange, PING→PONG,
    // NACK/STATE/ECHO to the plugin callbacks. A malformed frame is logged &
    // skipped, never thrown out of the loop (§4.3 / defensive requirement).
    public async Task ReceiveLoopAsync(Action<NackInfo> onNack, Action<ushort, byte[]> onState,
                                       CancellationToken token, Action<EchoInfo> onEcho = null)
    {
        _onNack = onNack; _onState = onState; _onEcho = onEcho;
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
                var info = new NackInfo
                {
                    Code = m.TryGetValue(SlopWire.KCode, out var c) ? (ushort)Convert.ToInt64(c) : (ushort)0,
                    Channel = m.TryGetValue(SlopWire.KChannelId, out var cc) ? (ushort)Convert.ToInt64(cc) : hdr.Channel,
                    Detail = m.TryGetValue(SlopWire.KDetail, out var d) ? d as string : null,
                    // RFC-001: the hub stamps this from the REFUSED FRAME'S HEADER
                    // seq. We send header.seq == intent_id, so this correlates
                    // directly with the intent we are waiting on.
                    IntentSeq = m.TryGetValue(SlopWire.KIntentSeq, out var s) ? Convert.ToInt64(s) : (long?)null,
                };
                _onNack?.Invoke(info);
                break;
            }
            case SlopWire.FState:
                // The payload is the packed snapshot; the plugin decodes it
                // against the catalog layout it adopted at readiness.
                _onState?.Invoke(hdr.Channel, payload);
                break;
            case SlopWire.FEcho:
            {
                // §9.3: the APPLIED (post-clamp) values, which are the only
                // values this client is ever allowed to display for a setting.
                var m = (Dictionary<long, object>)new CborReader(payload).Decode();
                var e = new EchoInfo
                {
                    Channel = m.TryGetValue(SlopWire.KChannelId, out var cc) ? (ushort)Convert.ToInt64(cc) : hdr.Channel,
                    IntentId = m.TryGetValue(SlopWire.KIntentId, out var ii) ? Convert.ToInt64(ii) : -1,
                    CfgGen = m.TryGetValue(SlopWire.KCfgGen, out var cg) ? Convert.ToInt64(cg) : -1,
                    Applied = m.TryGetValue(SlopWire.KApplied, out var ap) ? ap as Dictionary<long, object> : null,
                };
                _onEcho?.Invoke(e);
                break;
            }
            case SlopWire.FEvent:
                // §9.4: kind at key 33, kind-specific fields in the `body` (40)
                // sub-map, whose integer keys come from the CHANNEL'S catalog
                // schema — not the global key space. Nothing this plugin needs
                // to act on; logged so an operator can see it happened.
                try
                {
                    var m = (Dictionary<long, object>)new CborReader(payload).Decode();
                    long kind = m.TryGetValue(SlopWire.KEventKind, out var k) ? Convert.ToInt64(k) : -1;
                    bool hasBody = m.ContainsKey(SlopWire.KBody);
                    _log.Debug("EVENT channel=0x{0:X4} kind={1} body={2}", hdr.Channel, kind, hasBody ? "yes" : "no");
                }
                catch { /* §4.3: an event we cannot parse is not our problem */ }
                break;
            case SlopWire.FGoodbye:
            {
                ushort code = 0;
                try
                {
                    var m = (Dictionary<long, object>)new CborReader(payload).Decode();
                    if (m.TryGetValue(SlopWire.KCode, out var c)) code = (ushort)Convert.ToInt64(c);
                }
                catch { /* an empty GOODBYE is legal */ }
                _log.Info("hub sent GOODBYE: {0}", SlopWire.NackName(code));
                break;
            }
            case SlopWire.FBlobChunk:
                // Steady-state chunks mean a re-transfer we did not ask for
                // (or a late repair reply). Nothing to do — the catalog is
                // fetched inline before the receive loop starts.
                break;
            case SlopWire.FGrant:
            case SlopWire.FWelcome:
            case SlopWire.FPong:
            case SlopWire.FBeacon:
            case SlopWire.FPairGrant:
            case SlopWire.FHubSig:
            case SlopWire.FPublish:
            default:
                // Unknown-means-ignore (§4.3). Trust-plane frames (HUB_SIG,
                // PAIR_GRANT) are deliberately not acted on: this client is a
                // bearer-token/unverified peer and says so.
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
