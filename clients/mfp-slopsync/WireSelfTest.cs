// =============================================================================
// WireSelfTest — standalone golden-byte check for the SlopSync wire codec.
//
// It re-implements the exact encoder logic used by SlopSync.cs (CBOR writer,
// frame header, HELLO / SUBSCRIBE / GOODBYE / CLOCK / STREAM builders) with NO
// MultiFunPlayer / WPF dependencies, and byte-compares its output against golden
// hex derived by RUNNING tools/slopsync_probe.py's own builder functions
// (see scratchpad/gen_golden.py). This is the referee that proves the C# bytes
// match the live-verified Python probe.
//
// Build & run:  dotnet run --project clients/mfp-slopsync/WireSelfTest.csproj
// Exits 0 on all-pass, 1 on any mismatch.
//
// NOTE: the encoder methods below are a deliberate verbatim copy of SlopSync.cs's
// SlopWire/CborWriter. If you change the codec in SlopSync.cs, mirror it here and
// re-run — both must keep matching the probe.
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

    private static int Main()
    {
        Console.WriteLine("SlopSync wire self-test (golden bytes from slopsync_probe.py):");

        var inst = new byte[] { 0, 1, 2, 3, 4, 5, 6, 7 };

        var hello = Wire.BuildHello("probe", "slopsync_probe.py", inst, 0x0084, 100.0);
        Check("HELLO payload", hello,
            "A50101026570726F62650371736C6F7073796E635F70726F62652E7079044800010203040506070B81A20CFA42C800000F1884");

        var helloFrame = Wire.EncodeFrame(0x00, 0, hello, 0);
        Check("HELLO frame", helloFrame,
            "0000000000003300A50101026570726F62650371736C6F7073796E635F70726F62652E7079044800010203040506070B81A20CFA42C800000F1884");

        var clockReq = Wire.BuildClockRequest(0x11223344);
        Check("CLOCK request", clockReq, "44332211");

        var stream = Wire.BuildStreamBundle(0x00010203, new (ushort, double, double)[] { (0, 0.5, 1.7592918) });
        Check("STREAM bundle payload", stream, "03020100010000008813DF06");

        var streamFrame = Wire.EncodeFrame(0x0C, 0x0084, stream, 0);
        Check("STREAM frame", streamFrame, "0C00840000000C0003020100010000008813DF06");

        var sub = Wire.BuildSubscribe(new (ushort, double, byte)[] { (0x0003, 0.0, 3), (0x0080, 20.0, 2) });
        Check("SUBSCRIBE payload", sub, "A10A82A30CFA000000000D030F03A30CFA41A000000D020F1880");

        var goodbye = Wire.BuildGoodbye(0x0107);
        Check("GOODBYE payload", goodbye, "A110190107");

        Console.WriteLine();
        if (_fail == 0) { Console.WriteLine("ALL PASS"); return 0; }
        Console.WriteLine($"{_fail} FAILED"); return 1;
    }
}

// ---- verbatim copy of SlopSync.cs's SlopWire encoders (MFP-free) ------------
internal static class Wire
{
    public const int KProtoVer = 1, KClientKind = 2, KClientName = 3, KInstanceId = 4, KToken = 5;
    public const int KSubscriptions = 10, KPublishes = 11, KRateHz = 12, KPriority = 13;
    public const int KChannelId = 15, KCode = 16;

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
    {
        var w = new Cbor();
        int n = 4 + (token16 != null ? 1 : 0) + 1;
        w.Map(n);
        w.U(KProtoVer); w.U(1);
        w.U(KClientKind); w.T(clientKind);
        w.U(KClientName); w.T(clientName);
        w.U(KInstanceId); w.B(instanceId);
        if (token16 != null) { w.U(KToken); w.B(token16); }
        w.U(KPublishes);
        w.Arr(1);
        w.Map(2);
        w.U(KRateHz); w.F((float)publishRateHz);
        w.U(KChannelId); w.U(publishChannel);
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
    public byte[] ToArray() => _ms.ToArray();
}
