using System;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace factoryos_10x_shell.Services.Files
{
    internal static class AppPackageSignature
    {
        internal const string Publisher = "CN=TestPub";
        private const string IndirectData = "1.3.6.1.4.1.311.2.1.4";
        private const string Sha256 = "2.16.840.1.101.3.4.2.1";
        private const string Rsa = "1.2.840.113549.1.1.1";
        private const string Sha256Rsa = "1.2.840.113549.1.1.11";
        private static readonly byte[] Subject = new X500DistinguishedName(Publisher).RawData;

        internal static byte[] CreateCertificate(byte[] serial, byte[] publicKey, DateTimeOffset now, Func<byte[], byte[]> sign)
        {
            var algorithm = Algorithm(Sha256Rsa);
            var tbs = Seq(Der(0xA0, Int(2)), Integer(serial), algorithm, Subject,
                Seq(UtcTime(now.AddDays(-1)), UtcTime(now.AddYears(10))), Subject,
                Seq(Algorithm(Rsa), Der(3, Join(new byte[] { 0 }, publicKey))),
                Der(0xA3, Seq(
                    Seq(Oid("2.5.29.15"), Der(1, new byte[] { 0xFF }), Octets(Der(3, new byte[] { 7, 0x80 }))),
                    Seq(Oid("2.5.29.37"), Octets(Seq(Oid("1.3.6.1.5.5.7.3.3")))),
                    Seq(Oid("2.5.29.19"), Der(1, new byte[] { 0xFF }), Octets(Seq())))));
            return Seq(tbs, algorithm, Der(3, Join(new byte[] { 0 }, sign(tbs))));
        }

        internal static byte[] CreatePackageSignature(byte[] hashes, byte[] serial, byte[] certificate, Func<byte[], byte[]> sign,
            bool isBundle = false)
        {
            var sipId = isBundle ? new Guid("0f5f58b3-aade-4b9a-a434-95742d92eceb") :
                new Guid("0ac5df4b-ce07-4de2-b76e-23c839a09fd1");
            var sip = Seq(Int(0x01010000), Octets(sipId.ToByteArray()),
                Int(0), Int(0), Int(0), Int(0), Int(0));
            var indirectContent = Join(Seq(Oid("1.3.6.1.4.1.311.2.1.30"), sip),
                Seq(Algorithm(Sha256), Octets(hashes)));
            byte[] messageDigest;
            using (var sha = SHA256.Create()) messageDigest = sha.ComputeHash(indirectContent);

            // Authenticode signs the DER SET, while PKCS#7 carries it with an
            // implicit context tag. DER orders SET elements lexicographically.
            var attributes = new[]
            {
                Seq(Oid("1.2.840.113549.1.9.3"), Set(Oid(IndirectData))),
                Seq(Oid("1.2.840.113549.1.9.4"), Set(Octets(messageDigest))),
                Seq(Oid("1.3.6.1.4.1.311.2.1.11"), Set(Seq(Oid("1.3.6.1.4.1.311.2.1.21")))),
                Seq(Oid("1.3.6.1.4.1.311.2.1.12"), Set(Seq()))
            };
            Array.Sort(attributes, CompareDer);
            var signer = Seq(Int(1), Seq(Subject, Integer(serial)), Algorithm(Sha256),
                Der(0xA0, Join(attributes)), Algorithm(Rsa), Octets(sign(Set(attributes))));
            var signedData = Seq(Int(1), Set(Algorithm(Sha256)),
                Seq(Oid(IndirectData), Der(0xA0, Seq(indirectContent))),
                Der(0xA0, certificate), Set(signer));
            return Join(Encoding.ASCII.GetBytes("PKCX"),
                Seq(Oid("1.2.840.113549.1.7.2"), Der(0xA0, signedData)));
        }

        private static int CompareDer(byte[] left, byte[] right)
        {
            for (int i = 0; i < Math.Min(left.Length, right.Length); i++)
            {
                int order = left[i].CompareTo(right[i]);
                if (order != 0) return order;
            }
            return left.Length.CompareTo(right.Length);
        }

        private static byte[] Join(params byte[][] values) => values.SelectMany(value => value).ToArray();
        private static byte[] Seq(params byte[][] values) => Der(0x30, Join(values));
        private static byte[] Set(params byte[][] values) => Der(0x31, Join(values));
        private static byte[] Octets(byte[] bytes) => Der(4, bytes);
        private static byte[] Algorithm(string oid) => Seq(Oid(oid), Der(5, new byte[0]));
        private static byte[] UtcTime(DateTimeOffset time) =>
            Der(0x17, Encoding.ASCII.GetBytes(time.UtcDateTime.ToString("yyMMddHHmmss'Z'", CultureInfo.InvariantCulture)));

        private static byte[] Int(uint value) =>
            Integer(new[] { (byte)(value >> 24), (byte)(value >> 16), (byte)(value >> 8), (byte)value });

        private static byte[] Integer(byte[] bytes)
        {
            int start = 0;
            while (start < bytes.Length - 1 && bytes[start] == 0) start++;
            var value = bytes.Skip(start).ToArray();
            if ((value[0] & 0x80) != 0) value = Join(new byte[] { 0 }, value);
            return Der(2, value);
        }

        private static byte[] Oid(string oid)
        {
            var parts = oid.Split('.').Select(part => uint.Parse(part, CultureInfo.InvariantCulture)).ToArray();
            var bytes = new System.Collections.Generic.List<byte> { (byte)(parts[0] * 40 + parts[1]) };
            foreach (uint part in parts.Skip(2)) bytes.AddRange(Base128(part));
            return Der(6, bytes.ToArray());
        }

        private static byte[] Base128(uint number)
        {
            var bytes = new System.Collections.Generic.Stack<byte>();
            bytes.Push((byte)(number & 0x7F));
            while ((number >>= 7) != 0) bytes.Push((byte)(0x80 | (number & 0x7F)));
            return bytes.ToArray();
        }

        private static byte[] Der(byte tag, byte[] value)
        {
            int length = value.Length;
            if (length < 128) return Join(new[] { tag, (byte)length }, value);
            var lengthBytes = new System.Collections.Generic.Stack<byte>();
            for (; length != 0; length >>= 8) lengthBytes.Push((byte)length);
            return Join(new[] { tag, (byte)(0x80 | lengthBytes.Count) }, lengthBytes.ToArray(), value);
        }
    }
}
