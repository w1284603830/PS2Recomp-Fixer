param(
    [Parameter(Mandatory = $true)][string]$Module,     # e.g. build\pcsx2gs\Release\pcsx2gs.dll (its .pdb beside it)
    [Parameter(Mandatory = $true)][string[]]$Offsets   # module offsets from a [crash] stack, e.g. c079f a744a
)
# [DoD] Source file and line of module offsets (crash stacks in err.log) with dbghelp and the module's PDB.
$ErrorActionPreference = 'Stop'
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Dbg {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
  public struct IMAGEHLP_LINE64 { public uint SizeOfStruct; public IntPtr Key; public uint LineNumber; public IntPtr FileName; public ulong Address; }
  [DllImport("dbghelp.dll", SetLastError = true)] public static extern bool SymInitialize(IntPtr h, string path, bool invade);
  [DllImport("dbghelp.dll", SetLastError = true)] public static extern uint SymSetOptions(uint o);
  [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)] public static extern ulong SymLoadModuleEx(IntPtr h, IntPtr file, string image, string module, ulong baseAddr, uint size, IntPtr data, uint flags);
  [DllImport("dbghelp.dll", SetLastError = true)] public static extern bool SymGetLineFromAddr64(IntPtr h, ulong addr, out uint disp, ref IMAGEHLP_LINE64 line);
  [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)] public static extern bool SymFromAddr(IntPtr h, ulong addr, out ulong disp, IntPtr sym);
  public static string Where(IntPtr h, ulong addr) {
    var line = new IMAGEHLP_LINE64(); line.SizeOfStruct = (uint)Marshal.SizeOf(typeof(IMAGEHLP_LINE64)); uint d;
    string file = "?"; uint ln = 0;
    if (SymGetLineFromAddr64(h, addr, out d, ref line)) { file = Marshal.PtrToStringAnsi(line.FileName); ln = line.LineNumber; }
    IntPtr buf = Marshal.AllocHGlobal(88 + 512); for (int i = 0; i < 88 + 512; i++) Marshal.WriteByte(buf, i, 0);
    Marshal.WriteInt32(buf, 0, 88); Marshal.WriteInt32(buf, 80, 500); ulong sd; string name = "?";
    if (SymFromAddr(h, addr, out sd, buf)) name = Marshal.PtrToStringAnsi(buf + 84);
    Marshal.FreeHGlobal(buf);
    return name + "  " + file + ":" + ln;
  }
}
'@
$h = [IntPtr]::new(0x1D0D)
[Dbg]::SymSetOptions(0x2 -bor 0x10) | Out-Null   # SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
$full = (Resolve-Path $Module).Path
[Dbg]::SymInitialize($h, (Split-Path $full), $false) | Out-Null
$base = [UInt64]0x10000000
[Dbg]::SymLoadModuleEx($h, [IntPtr]::Zero, $full, $null, $base, 0, [IntPtr]::Zero, 0) | Out-Null
foreach ($o in $Offsets) {
    $off = [Convert]::ToUInt64(($o -replace '^0x', ''), 16)
    '{0,8}  {1}' -f $o, [Dbg]::Where($h, $base + $off)
}
