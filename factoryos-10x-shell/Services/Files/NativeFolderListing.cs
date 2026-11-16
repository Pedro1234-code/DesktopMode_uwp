using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace factoryos_10x_shell.Services.Files
{
    internal static class NativeFolderListing
    {
        private const string FileApi = "api-ms-win-core-file-l1-1-0.dll";
        private static readonly IntPtr InvalidHandle = new IntPtr(-1);
        private const int InvalidParameter = 87;
        private const int NoMoreFiles = 18;
        private const uint LargeFetch = 2;
        private const int BasicInfo = 1;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct FindData
        {
            public FileAttributes Attributes;
            public uint CreationLow, CreationHigh;
            public uint AccessLow, AccessHigh;
            public uint WriteLow, WriteHigh;
            public uint SizeHigh, SizeLow;
            public uint Reserved0, Reserved1;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string Name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)] public string AlternateName;
        }

        [DllImport("api-ms-win-core-file-fromapp-l1-1-0.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr FindFirstFileExFromAppW(string pattern, int infoLevel, out FindData data,
            int searchOperation, IntPtr filter, uint flags);

        [DllImport(FileApi, CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FindNextFileW(IntPtr handle, out FindData data);

        [DllImport(FileApi, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FindClose(IntPtr handle);

        [DllImport(FileApi, ExactSpelling = true)]
        private static extern uint GetLogicalDrives();

        internal static IReadOnlyList<Entry> TryRead(string path)
        {
            if (string.IsNullOrEmpty(path)) return null;

            try
            {
                var entries = new List<Entry>();
                string pattern = Path.Combine(path, "*");
                IntPtr handle = FindFirstFileExFromAppW(pattern, BasicInfo, out var data, 0, IntPtr.Zero, LargeFetch);
                if (handle == InvalidHandle && Marshal.GetLastWin32Error() == InvalidParameter)
                    handle = FindFirstFileExFromAppW(pattern, BasicInfo, out data, 0, IntPtr.Zero, 0);
                if (handle == InvalidHandle)
                    return null;

                try
                {
                    do
                    {
                        if (data.Name == "." || data.Name == "..") continue;
                        long fileTime = (long)(((ulong)data.WriteHigh << 32) | data.WriteLow);
                        entries.Add(new Entry
                        {
                            Name = data.Name,
                            IsDirectory = (data.Attributes & FileAttributes.Directory) != 0,
                            Size = ((ulong)data.SizeHigh << 32) | data.SizeLow,
                            DateModified = fileTime > 0 && fileTime <= DateTime.MaxValue.ToFileTimeUtc()
                                ? new DateTimeOffset(DateTime.FromFileTimeUtc(fileTime)) : default(DateTimeOffset)
                        });
                    }
                    while (FindNextFileW(handle, out data));
                    return Marshal.GetLastWin32Error() == NoMoreFiles ? entries : null;
                }
                finally
                {
                    FindClose(handle);
                }
            }
            catch (DllNotFoundException) { return null; }
            catch (EntryPointNotFoundException) { return null; }
        }

        internal static IReadOnlyList<string> ReadDriveRoots()
        {
            uint mask;
            try { mask = GetLogicalDrives(); }
            catch (DllNotFoundException) { return Array.Empty<string>(); }
            catch (EntryPointNotFoundException) { return Array.Empty<string>(); }

            var roots = new List<string>();
            for (int index = 0; index < 26; index++)
                if ((mask & (1u << index)) != 0)
                    roots.Add((char)('A' + index) + @":\");
            return roots;
        }

        internal sealed class Entry
        {
            public string Name { get; set; }
            public bool IsDirectory { get; set; }
            public ulong Size { get; set; }
            public DateTimeOffset DateModified { get; set; }
        }
    }
}
