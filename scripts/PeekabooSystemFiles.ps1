# Dot-sourced by package_peekaboo.ps1 (a public package) and export_public.ps1 (the public
# source): neither may carry a copyrighted Amiga system file.

# What a file is, if it is an Amiga system file: by name, then by content - a Kickstart's header
# (plain or Amiga Forever's encrypted one) at a Kickstart's size, or an ADF whose volume is a
# Workbench disk. $null for anything else.
function Get-SystemFileKind([IO.FileInfo]$f) {
    $n = $f.Name.ToLowerInvariant()
    if ($n.EndsWith(".key")) { return "a ROM key" }
    if ($n -match '\.(hdf|hdz|vhd)$') { return "a hardfile" }
    # The presets' names (kick34005.A500) and Amiga Forever's / TOSEC's .rom - not "Kickstart Disk
    # A1000 ...bin", which is a 1 KB boot block in bootblocks\
    if ($n -match '^kick\d{5}\.' -or $n.EndsWith(".rom")) { return "named as a ROM" }
    $len = $f.Length
    if (@(262144, 524288, 1048576, 262155, 524299, 1048587) -contains $len) {
        $head = New-Object byte[] 11
        $fs = [IO.File]::OpenRead($f.FullName)
        try { [void]$fs.Read($head, 0, 11) } finally { $fs.Close() }
        $magic = [BitConverter]::ToString($head, 0, 4)
        if (@("11-11-4E-F9", "11-14-4E-F9", "11-16-4E-F9") -contains $magic -or
            [Text.Encoding]::ASCII.GetString($head) -eq "AMIROMTYPE1") { return "a Kickstart image" }
    }
    if ($n.EndsWith(".adf") -and ($len -eq 901120 -or $len -eq 1802240)) {
        # The root block's BCPL volume name: block 880 (DD) or 1760 (HD), offset 432.
        $root = if ($len -eq 901120) { 880 } else { 1760 }
        $buf = New-Object byte[] 32
        $fs = [IO.File]::OpenRead($f.FullName)
        try { [void]$fs.Seek($root * 512 + 432, 0); [void]$fs.Read($buf, 0, 32) } finally { $fs.Close() }
        $nameLen = [Math]::Min([int]$buf[0], 30)
        $volume = [Text.Encoding]::ASCII.GetString($buf, 1, $nameLen)
        if ($volume -match '^(Workbench|Install|Extras|Storage|Locale|Fonts|System)') {
            return "a Workbench disk ($volume)"
        }
    }
    return $null
}

# Every system file under $root, as "relative\path: kind" lines.
function Find-SystemFiles([string]$root) {
    $found = @()
    foreach ($f in Get-ChildItem -Recurse -File -LiteralPath $root) {
        $kind = Get-SystemFileKind $f
        if ($kind) { $found += "  $($f.FullName.Substring($root.Length).TrimStart('\')): $kind" }
    }
    return $found
}
