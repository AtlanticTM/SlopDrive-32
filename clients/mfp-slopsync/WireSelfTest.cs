// =============================================================================
// WireSelfTest — standalone golden-byte check for the SlopSync wire codec.
//
// It re-implements the exact encoder logic used by SlopSync.cs (CBOR writer,
// frame header, HELLO / SUBSCRIBE / GOODBYE / CLOCK / STREAM / BLOB_REQ /
// INTENT builders and the BLOB_CHUNK header decoder) with NO MultiFunPlayer /
// WPF dependencies, and byte-compares its output against golden hex derived by
// RUNNING tools/slopsync_probe.py's own CBOR primitives (see
// scratchpad/gen_golden_m5d.py). This is the referee that proves the C# bytes
// match the live-verified Python probe.
//
// Build & run:  dotnet run --project clients/mfp-slopsync/WireSelfTest.csproj
// Exits 0 on all-pass, 1 on any mismatch.
//
// NOTE: the encoder methods below are a deliberate verbatim copy of SlopSync.cs's
// SlopWire/CborWriter. If you change the codec in SlopSync.cs, mirror it here and
// re-run — both must keep matching the probe.
//
// M5d MOVED THE HELLO GOLDENS ON PURPOSE. Adding RFC-006's `subscriptions` (10)
// wish to HELLO changes its bytes; that coupling is documented in RFC-006 and
// this file is where it is enforced. The publish-only golden is KEPT as a
// regression guard, so a hub or client that still speaks the old shape can be
// checked against it.
//
// v0.4.0 MOVED THE SEGMENTS GOLDEN AGAIN (RFC-013 + RFC-030). The 0x0085 (now
// 0x2101, RFC-047) wish
// now declares its honest rate (5 Hz, was an over-declared 30) plus `burst`
// (42) and `curve_family` (45) in the wish-entry map — keys ascending
// 12<15<42<45. The pre-RFC-013 2-key entry shape is KEPT as a regression
// guard (the rate-only BuildHello overload must stay byte-identical). Goldens
// derived from tools/slopsync_probe.py's primitives via
// scratchpad/gen_golden_rfc013_030.py.
// =============================================================================
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

internal static class Program
{
    private static int _fail;

    private static void Check(string name, byte[] actual, string expectedHex)
    {
        string got = Convert.ToHexString(actual);
        string want = expectedHex.Replace(" ", "").ToUpperInvariant();
        bool ok = got == want;
        Console.WriteLine($"  [{(ok ? "PASS" : "FAIL")}] {name}");
        if (!ok)
        {
            Console.WriteLine($"        expected: {want}");
            Console.WriteLine($"        actual:   {got}");
            _fail++;
        }
    }

    private static void CheckEq(string name, object actual, object expected)
    {
        bool ok = Equals(actual, expected);
        Console.WriteLine($"  [{(ok ? "PASS" : "FAIL")}] {name}");
        if (!ok)
        {
            Console.WriteLine($"        expected: {expected}");
            Console.WriteLine($"        actual:   {actual}");
            _fail++;
        }
    }

    private static int Main()
    {
        Console.WriteLine("SlopSync wire self-test (golden bytes from slopsync_probe.py):");

        var inst = new byte[] { 0, 1, 2, 3, 4, 5, 6, 7 };
        // The frozen mini-catalog etag — a stable 8-byte value to exercise the
        // etag paths with, not a live device's.
        var etag = new byte[] { 0x21, 0xCB, 0x26, 0xC9, 0x4F, 0xB3, 0x88, 0xB5 };

        // ---- HELLO: publish-only shape (pre-M5d) — regression guard ---------
        var hello = Wire.BuildHello("probe", "slopsync_probe.py", inst, 0x2100, 100.0);
        Check("HELLO payload (publish-only, unchanged)", hello,
            "A50101026570726F62650371736C6F7073796E635F70726F62652E7079044800010203040506070B81A20CFA42C800000F192100");

        var helloFrame = Wire.EncodeFrame(0x00, 0, hello, 0);
        Check("HELLO frame (publish-only)", helloFrame,
            "0000000000003400A50101026570726F62650371736C6F7073796E635F70726F62652E7079044800010203040506070B81A20CFA42C800000F192100");

        // ---- HELLO: the plugin's ACTUAL M5d shape ---------------------------
        // subscriptions(10) rides in HELLO now: safety(0x0003) on-change
        // critical + motion(0x0080) @20 Hz elevated, then the publish wish.
        // Key order 1<2<3<4<10<11; wish-entry order 12<13<15 and 12<15.
        var subs = new (ushort, double, byte)[] { (0x0003, 0.0, 3), (0x0080, 20.0, 2) };
        var helloMfp = Wire.BuildHello("mfp", "MultiFunPlayer SlopSync", inst,
            new (ushort, double)[] { (0x2100, 50.0) }, null, subs);
        Check("HELLO payload (mfp Samples: subs + 1 publish)", helloMfp,
            "A6010102636D667003774D756C746946756E506C6179657220536C6F7053796E63044800010203040506070A82A30CFA000000000D030F03A30CFA41A000000D020F18800B81A20CFA424800000F192100");

        Check("HELLO frame (mfp Samples)", Wire.EncodeFrame(0x00, 0, helloMfp, 0),
            "0000000000005100A6010102636D667003774D756C746946756E506C6179657220536C6F7053796E63044800010203040506070A82A30CFA000000000D030F03A30CFA41A000000D020F18800B81A20CFA424800000F192100");

        // Rate-only wish entries (pre-RFC-013 2-key {12,15} shape) — kept as a
        // regression guard: the rate-only BuildHello overload must keep
        // producing exactly these bytes.
        var helloSeg = Wire.BuildHello("mfp", "MultiFunPlayer SlopSync", inst,
            new (ushort, double)[] { (0x2100, 50.0), (0x2101, 30.0) }, null, subs);
        Check("HELLO payload (rate-only wish entries, pre-RFC-013 regression guard)", helloSeg,
            "A6010102636D667003774D756C746946756E506C6179657220536C6F7053796E63044800010203040506070A82A30CFA000000000D030F03A30CFA41A000000D020F18800B82A20CFA424800000F192100A20CFA41F000000F192101");

        // ---- HELLO: the plugin's ACTUAL v0.4.0 Segments shape ----------------
        // RFC-013 honest wish + RFC-030 curve declaration on the 0x2101 entry
        // (RFC-047 grid; was 0x0085):
        // {12:rate 5.0, 15:0x2101, 42:burst 25.0, 45:curve_family 1} — keys
        // ascending 12<15<42<45; the 0x2100 fallback entry (was 0x0084) stays
        // the 2-key map (burst<=0 and family 0 are OMITTED, never encoded).
        var helloSegV4 = Wire.BuildHello("mfp", "MultiFunPlayer SlopSync", inst,
            new (ushort, double, double, byte)[] { (0x2100, 50.0, 0.0, 0), (0x2101, 5.0, 25.0, 1) },
            null, subs);
        Check("HELLO payload (mfp Segments v0.4.0: honest rate + burst + curve_family)", helloSegV4,
            "A6010102636D667003774D756C746946756E506C6179657220536C6F7053796E63044800010203040506070A82A30CFA000000000D030F03A30CFA41A000000D020F18800B82A20CFA424800000F192100A40CFA40A000000F192101182AFA41C80000182D01");

        // ---- HELLO: RFC-015 cached-etag fast path ---------------------------
        // catalog_etag(8) sits between instance_id(4) and subscriptions(10).
        var helloEtag = Wire.BuildHello("mfp", "MultiFunPlayer SlopSync", inst,
            new (ushort, double)[] { (0x2100, 50.0) }, null, subs, etag);
        Check("HELLO payload (cached etag + subs + publish)", helloEtag,
            "A7010102636D667003774D756C746946756E506C6179657220536C6F7053796E6304480001020304050607084821CB26C94FB388B50A82A30CFA000000000D030F03A30CFA41A000000D020F18800B81A20CFA424800000F192100");

        // Every optional at once: token(5) then etag(8) then subs(10).
        var helloAll = Wire.BuildHello("mfp", "MultiFunPlayer SlopSync", inst,
            new (ushort, double)[] { (0x2100, 50.0) }, Encoding.UTF8.GetBytes("1234"), subs, etag);
        Check("HELLO payload (token + etag + subs + publish)", helloAll,
            "A8010102636D667003774D756C746946756E506C6179657220536C6F7053796E6304480001020304050607054431323334084821CB26C94FB388B50A82A30CFA000000000D030F03A30CFA41A000000D020F18800B81A20CFA424800000F192100");

        var clockReq = Wire.BuildClockRequest(0x11223344);
        Check("CLOCK request", clockReq, "44332211");

        var stream = Wire.BuildStreamBundle(0x00010203, new (ushort, double, double)[] { (0, 0.5, 1.7592918) });
        Check("STREAM bundle payload", stream, "03020100010000008813DF06");

        var streamFrame = Wire.EncodeFrame(0x0C, 0x2100, stream, 0);
        Check("STREAM frame", streamFrame, "0C00002100000C0003020100010000008813DF06");

        var sub = Wire.BuildSubscribe(new (ushort, double, byte)[] { (0x0003, 0.0, 3), (0x0080, 20.0, 2) });
        Check("SUBSCRIBE payload", sub, "A10A82A30CFA000000000D030F03A30CFA41A000000D020F1880");

        // Mid-session SUBSCRIBE for the ROLE-LOCATED limits channel. 0x0081 is
        // the value on THIS device only — the plugin gets it from the catalog,
        // the test just needs a concrete number to encode.
        Check("SUBSCRIBE payload (role-located channel, on-change normal)",
            Wire.BuildSubscribe(new (ushort, double, byte)[] { (0x0081, 0.0, 1) }),
            "A10A81A30CFA000000000D010F1881");

        // GOODBYE's code is NORMAL_CLOSURE from the NackCode vocabulary (§6.8),
        // not a private literal.
        var goodbye = Wire.BuildGoodbye(Wire.NackNormalClosure);
        Check("GOODBYE payload (NackCode NORMAL_CLOSURE)", goodbye, "A110190107");

        // ---- Segments mode (0x2101, was 0x0085) golden bytes ----------------
        // Hand-derived from the locked wire contract, same STREAM framing as
        // 0x2100 (was 0x0084). t_base 0x00010203, single sample at off 0:
        //   [t_base:03 02 01 00][n:01][rsv:00][off:00 00]
        //   [target 0.5 → 5000=0x1388 → 88 13]
        //   [duration 900 → 0x0384 → 84 03]
        //   [end_vel SENTINEL → INT16_MIN 0x8000 LE → 00 80]
        var seg = Wire.BuildSegmentBundle(0x00010203,
            new (ushort, double, int, double, bool)[] { (0, 0.5, 900, 0.0, true) });
        Check("SEGMENT bundle payload (sentinel end_vel)", seg, "0302010001000000881384030080");

        var segFrame = Wire.EncodeFrame(0x0C, 0x2101, seg, 0);
        Check("SEGMENT frame (channel 0x2101, len 14)", segFrame,
            "0C00012100000E000302010001000000881384030080");

        // Real end_vel path: target 0.25 → 2500=0x09C4, duration 500=0x01F4,
        // end_vel 1.5 norm/s → 1500=0x05DC (LE DC 05) — proves it never collides
        // with the sentinel and is NOT written as 0x8000.
        var segReal = Wire.BuildSegmentBundle(0x00010203,
            new (ushort, double, int, double, bool)[] { (0, 0.25, 500, 1.5, false) });
        Check("SEGMENT bundle payload (end_vel 1.5)", segReal, "0302010001000000C409F401DC05");

        // ---- PING keepalive frame (raw, empty payload; §6.5) ----------------
        // Header only: type 0x03, flags 0, channel 0, seq 0, len 0.
        var ping = Wire.EncodeFrame(0x03, 0, Array.Empty<byte>(), 0);
        Check("PING frame (raw, empty)", ping, "0300000000000000");

        // ---- BuildHello refactor invariant ----------------------------------
        // The single-wish overload must be byte-identical to a list-of-one, so
        // Samples mode's HELLO bytes are unchanged by the multi-wish refactor.
        var helloListOfOne = Wire.BuildHello("probe", "slopsync_probe.py", inst,
            new (ushort, double)[] { (0x2100, 100.0) });
        Check("HELLO single-wish == list-of-one", helloListOfOne, Convert.ToHexString(hello));

        // ====================================================================
        // v1.0 BLOB TRANSFER (RFC-021) — CATALOG_REQ/CHUNK (0x09/0x0A) are
        // RETIRED and their numbers burned; catalog is blob namespace 0.
        // ====================================================================
        // A bare catalog request is the EMPTY CBOR map: ns 0 is the default and
        // store_id/slot are absent by rule, so generalizing transfer cost the
        // common case exactly zero bytes.
        Check("BLOB_REQ payload (bare catalog = empty map)", Wire.BuildCatalogRequest(), "A0");
        Check("BLOB_REQ frame (bare catalog)", Wire.EncodeFrame(0x1A, 0, Wire.BuildCatalogRequest(), 0),
            "1A00000000000100A0");
        Check("BLOB_REQ payload (repair chunks 2,5,9)", Wire.BuildCatalogRepair(new[] { 2, 5, 9 }),
            "A1181B83020509");
        Check("BLOB_REQ payload (ns=1 store 1 slot 0)", Wire.BuildBlobReq(1, 1, 0),
            "A11826A3010102010300");

        // ---- CATALOG_READY (0x19) — RAW frame, payload = the 8 etag bytes ---
        Check("CATALOG_READY frame (raw, 8 etag bytes)", Wire.EncodeFrame(0x19, 0, etag, 0),
            "190000000000080021CB26C94FB388B5");

        // ---- BLOB_CHUNK (0x1B) 14-byte identity header ----------------------
        // h2c, so we only DECODE it — the golden proves this client reads the
        // v1.0 header layout (was 8 bytes on the retired CATALOG_CHUNK).
        var bcHex = "000000000700030036002B280000AABB";
        var bc = Convert.FromHexString(bcHex);
        if (!Wire.TryDecodeBlobChunk(bc, out var chunk))
        {
            Console.WriteLine("  [FAIL] BLOB_CHUNK header decode (returned false)");
            _fail++;
        }
        else
        {
            CheckEq("BLOB_CHUNK header: ns", (int)chunk.Ns, 0);
            CheckEq("BLOB_CHUNK header: generation", (int)chunk.Generation, 7);
            CheckEq("BLOB_CHUNK header: chunk_index", (int)chunk.ChunkIndex, 3);
            CheckEq("BLOB_CHUNK header: chunk_count", (int)chunk.ChunkCount, 54);
            CheckEq("BLOB_CHUNK header: total_bytes", (long)chunk.TotalBytes, 10283L);
            CheckEq("BLOB_CHUNK header: body length", chunk.Bytes.Length, 2);
            Check("BLOB_CHUNK header: body bytes", chunk.Bytes, "AABB");
        }
        // A payload shorter than the header must be REFUSED, not mis-parsed.
        CheckEq("BLOB_CHUNK header: 13-byte payload refused",
            Wire.TryDecodeBlobChunk(new byte[13], out _), false);

        // ====================================================================
        // INTENT (§9.3) — and RFC-001's correlation rule.
        // ====================================================================
        // The hub stamps NACK.intent_seq from the REFUSED FRAME'S HEADER seq,
        // so the plugin sets header.seq = intent_id. These two frames are the
        // proof that both correlation keys name the same number.
        var iWin = Wire.BuildIntent(0x0101, 7,
            new (int, byte[])[] { (1, Wire.CborF32(20.0)), (2, Wire.CborF32(150.0)) });
        Check("INTENT payload (config-set window 20/150)", iWin,
            "A30F190101120714A201FA41A0000002FA43160000");
        Check("INTENT frame (config-set, header seq == intent_id 7)",
            Wire.EncodeFrame(0x0D, 0x0101, iWin, 7),
            "0D00010107001500A30F190101120714A201FA41A0000002FA43160000");

        var iHome = Wire.BuildIntent(0x0103, 3, new (int, byte[])[] { (1, Wire.CborUInt(1)) });
        Check("INTENT payload (home op 1)", iHome, "A30F190103120314A10101");
        Check("INTENT frame (home, header seq == intent_id 3)",
            Wire.EncodeFrame(0x0D, 0x0103, iHome, 3),
            "0D00030103000B00A30F190103120314A10101");

        Console.WriteLine();
        if (_fail == 0) { Console.WriteLine("ALL PASS"); return 0; }
        Console.WriteLine($"{_fail} FAILED"); return 1;
    }
}

// ---- verbatim copy of SlopSync.cs's SlopWire encoders (MFP-free) ------------
internal static class Wire
{
    public const int KProtoVer = 1, KClientKind = 2, KClientName = 3, KInstanceId = 4, KToken = 5;
    public const int KCatalogEtag = 8, KSubscriptions = 10, KPublishes = 11, KRateHz = 12, KPriority = 13;
    public const int KChannelId = 15, KCode = 16, KIntentId = 18, KValue = 20;
    public const int KChunks = 27, KBlob = 38;
    public const int KBurst = 42, KCurveFamily = 45;   // RFC-013 / RFC-030 wish-entry keys
    public const byte CurveUnspecified = 0;
    public const int BlobKNs = 1, BlobKStoreId = 2, BlobKSlot = 3, BlobKGeneration = 4;
    public const int BlobNsCatalog = 0;
    public const int BlobChunkHeaderBytes = 14;
    public const ushort NackNormalClosure = 0x0107;

    public static byte[] EncodeFrame(byte type, ushort channel, ReadOnlySpan<byte> payload, ushort seq = 0, byte flags = 0)
    {
        var buf = new byte[8 + payload.Length];
        buf[0] = type; buf[1] = flags;
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(2), channel);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(4), seq);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(6), (ushort)payload.Length);
        payload.CopyTo(buf.AsSpan(8));
        return buf;
    }

    public static byte[] BuildHello(string clientKind, string clientName, byte[] instanceId,
                                    ushort publishChannel, double publishRateHz, byte[] token16 = null)
        => BuildHello(clientKind, clientName, instanceId,
                      new (ushort ch, double rate)[] { (publishChannel, publishRateHz) }, token16);

    public static byte[] BuildHello(string clientKind, string clientName, byte[] instanceId,
                                    IReadOnlyList<(ushort ch, double rate)> publishes, byte[] token16 = null,
                                    IReadOnlyList<(ushort ch, double rate, byte prio)> subscribes = null,
                                    byte[] catalogEtag = null)
        => BuildHello(clientKind, clientName, instanceId,
                      publishes.Select(p => (p.ch, p.rate, 0.0, (byte)0)).ToList(),
                      token16, subscribes, catalogEtag);

    public static byte[] BuildHello(string clientKind, string clientName, byte[] instanceId,
                                    IReadOnlyList<(ushort ch, double rate, double burst, byte curveFamily)> publishes,
                                    byte[] token16 = null,
                                    IReadOnlyList<(ushort ch, double rate, byte prio)> subscribes = null,
                                    byte[] catalogEtag = null)
    {
        bool hasSubs = subscribes != null && subscribes.Count > 0;
        bool hasEtag = catalogEtag != null && catalogEtag.Length > 0;

        var w = new Cbor();
        int n = 4 + (token16 != null ? 1 : 0) + (hasEtag ? 1 : 0) + (hasSubs ? 1 : 0) + 1;
        w.Map(n);
        w.U(KProtoVer); w.U(1);
        w.U(KClientKind); w.T(clientKind);
        w.U(KClientName); w.T(clientName);
        w.U(KInstanceId); w.B(instanceId);
        if (token16 != null) { w.U(KToken); w.B(token16); }
        if (hasEtag) { w.U(KCatalogEtag); w.B(catalogEtag); }
        if (hasSubs)
        {
            w.U(KSubscriptions);
            w.Arr(subscribes.Count);
            foreach (var (ch, rate, prio) in subscribes)
            {
                w.Map(3);
                w.U(KRateHz); w.F((float)rate);
                w.U(KPriority); w.U(prio);
                w.U(KChannelId); w.U(ch);
            }
        }
        w.U(KPublishes);
        w.Arr(publishes.Count);
        foreach (var (ch, rate, burst, curveFamily) in publishes)
        {
            int entries = 2 + (burst > 0 ? 1 : 0) + (curveFamily != CurveUnspecified ? 1 : 0);
            w.Map(entries);                           // keys ascending: 12 < 15 < 42 < 45
            w.U(KRateHz); w.F((float)rate);
            w.U(KChannelId); w.U(ch);
            if (burst > 0) { w.U(KBurst); w.F((float)burst); }
            if (curveFamily != CurveUnspecified) { w.U(KCurveFamily); w.U(curveFamily); }
        }
        return w.ToArray();
    }

    public static byte[] BuildSubscribe(IEnumerable<(ushort ch, double rate, byte prio)> wishes)
    {
        var list = wishes.ToList();
        var w = new Cbor();
        w.Map(1);
        w.U(KSubscriptions);
        w.Arr(list.Count);
        foreach (var (ch, rate, prio) in list)
        {
            w.Map(3);
            w.U(KRateHz); w.F((float)rate);
            w.U(KPriority); w.U(prio);
            w.U(KChannelId); w.U(ch);
        }
        return w.ToArray();
    }

    public static byte[] BuildGoodbye(ushort code)
    {
        var w = new Cbor();
        w.Map(1);
        w.U(KCode); w.U(code);
        return w.ToArray();
    }

    public static byte[] BuildClockRequest(uint t0)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, t0);
        return b;
    }

    // ---- BLOB_REQ (0x1A, §8.4 / RFC-021) — key order chunks(27) < blob(38) --
    public static byte[] BuildBlobReq(int ns = BlobNsCatalog, int? storeId = null, int? slot = null,
                                      int? generation = null, IReadOnlyList<int> chunks = null)
    {
        bool hasChunks = chunks != null && chunks.Count > 0;
        var sub = new List<(int key, long val)>();
        if (ns != BlobNsCatalog) sub.Add((BlobKNs, ns));
        if (storeId.HasValue) sub.Add((BlobKStoreId, storeId.Value));
        if (slot.HasValue) sub.Add((BlobKSlot, slot.Value));
        if (generation.HasValue) sub.Add((BlobKGeneration, generation.Value));

        var w = new Cbor();
        w.Map((hasChunks ? 1 : 0) + (sub.Count > 0 ? 1 : 0));
        if (hasChunks)
        {
            w.U(KChunks);
            w.Arr(chunks.Count);
            foreach (var i in chunks) w.U(i);
        }
        if (sub.Count > 0)
        {
            w.U(KBlob);
            w.Map(sub.Count);
            foreach (var (k, v) in sub) { w.U(k); w.U(v); }
        }
        return w.ToArray();
    }

    public static byte[] BuildCatalogRequest() => BuildBlobReq();
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

    // ---- INTENT (§9.3) — {15:channel, 18:intent_id, 20:{value}} -------------
    public static byte[] BuildIntent(ushort channelId, long intentId,
                                     IReadOnlyList<(int key, byte[] encoded)> valueFields)
    {
        var w = new Cbor();
        w.Map(3);
        w.U(KChannelId); w.U(channelId);
        w.U(KIntentId); w.U(intentId);
        w.U(KValue);
        w.Map(valueFields.Count);
        foreach (var (key, enc) in valueFields) { w.U(key); w.Raw(enc); }
        return w.ToArray();
    }

    public static byte[] CborF32(double v) { var w = new Cbor(); w.F((float)v); return w.ToArray(); }
    public static byte[] CborUInt(long v) { var w = new Cbor(); w.U(v); return w.ToArray(); }

    public static byte[] BuildStreamBundle(uint tBase, IReadOnlyList<(ushort off, double target, double vel)> samples)
    {
        int n = samples.Count;
        var buf = new byte[6 + n * 2 + n * 4];
        int p = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(p), tBase); p += 4;
        buf[p++] = (byte)n; buf[p++] = 0;
        for (int i = 0; i < n; i++) { BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), samples[i].off); p += 2; }
        for (int i = 0; i < n; i++)
        {
            int rawT = Math.Clamp((int)Math.Round(samples[i].target * 10000.0), 0, 65535);
            int rawV = Math.Clamp((int)Math.Round(samples[i].vel * 1000.0), -32768, 32767);
            BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), (ushort)rawT); p += 2;
            BinaryPrimitives.WriteInt16LittleEndian(buf.AsSpan(p), (short)rawV); p += 2;
        }
        return buf;
    }

    // Mirror of SlopWire.BuildSegmentBundle (0x2101, was 0x0085): 6-byte samples
    // {target:u16 LE ×10000, duration:u16 LE, end_vel:i16 LE ×1000}. Sentinel
    // encodes INT16_MIN; a real end_vel clamps ±32767 so it can't hit the sentinel.
    public static byte[] BuildSegmentBundle(uint tBase, IReadOnlyList<(ushort off, double target, int durationMs, double endVel, bool sentinel)> samples)
    {
        int n = samples.Count;
        var buf = new byte[6 + n * 2 + n * 6];
        int p = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(p), tBase); p += 4;
        buf[p++] = (byte)n; buf[p++] = 0;
        for (int i = 0; i < n; i++) { BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), samples[i].off); p += 2; }
        for (int i = 0; i < n; i++)
        {
            int rawT = Math.Clamp((int)Math.Round(samples[i].target * 10000.0), 0, 65535);
            int rawD = Math.Clamp(samples[i].durationMs, 1, 65535);
            short rawV = samples[i].sentinel
                ? short.MinValue
                : (short)Math.Clamp((int)Math.Round(samples[i].endVel * 1000.0), -32767, 32767);
            BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), (ushort)rawT); p += 2;
            BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(p), (ushort)rawD); p += 2;
            BinaryPrimitives.WriteInt16LittleEndian(buf.AsSpan(p), rawV); p += 2;
        }
        return buf;
    }
}

internal sealed class Cbor
{
    private readonly MemoryStream _ms = new();
    private void Head(int major, ulong v)
    {
        int ib0 = major << 5;
        if (v <= 23) _ms.WriteByte((byte)(ib0 | (int)v));
        else if (v <= 0xFF) { _ms.WriteByte((byte)(ib0 | 24)); _ms.WriteByte((byte)v); }
        else if (v <= 0xFFFF) { _ms.WriteByte((byte)(ib0 | 25)); BE(v, 2); }
        else if (v <= 0xFFFFFFFF) { _ms.WriteByte((byte)(ib0 | 26)); BE(v, 4); }
        else { _ms.WriteByte((byte)(ib0 | 27)); BE(v, 8); }
    }
    private void BE(ulong v, int n) { for (int i = n - 1; i >= 0; i--) _ms.WriteByte((byte)((v >> (8 * i)) & 0xFF)); }
    public void U(long v) => Head(0, (ulong)v);
    public void F(float f) { _ms.WriteByte(0xFA); Span<byte> t = stackalloc byte[4]; BinaryPrimitives.WriteSingleBigEndian(t, f); _ms.Write(t); }
    public void T(string s) { var b = Encoding.UTF8.GetBytes(s); Head(3, (ulong)b.Length); _ms.Write(b, 0, b.Length); }
    public void B(ReadOnlySpan<byte> b) { Head(2, (ulong)b.Length); _ms.Write(b); }
    public void Arr(int c) => Head(4, (ulong)c);
    public void Map(int c) => Head(5, (ulong)c);
    public void Raw(ReadOnlySpan<byte> encoded) => _ms.Write(encoded);
    public byte[] ToArray() => _ms.ToArray();
}
