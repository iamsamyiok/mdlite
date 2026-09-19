$tdir = "E:\Desktop\mdlite-src\testnotes"

# gdb script: run, on crash print backtrace
Set-Content -Path "$tdir\gdb_cmds.txt" -Value "set pagination off`nset confirm off`nrun`nbt full`ninfo registers rip`nquit"

# fresh notes
Set-Content -Path "$tdir\A.md" -Value "# A`n`nsee [[B]] here`n" -Encoding UTF8
Set-Content -Path "$tdir\B.md" -Value "# B`n`ncontent of B`n" -Encoding UTF8
Remove-Item "$tdir\.mdlite" -Recurse -Force -ErrorAction SilentlyContinue

# launch under gdb
$gdb = Start-Process -FilePath "E:\download6\winlibs\mingw64\bin\gdb.exe" `
    -ArgumentList "-q","-batch","-x","$tdir\gdb_cmds.txt","--args","$tdir\MDLite-dbg.exe","$tdir\A.md" `
    -RedirectStandardOutput "$tdir\gdb_out.log" -RedirectStandardError "$tdir\gdb_err.log" `
    -PassThru
Start-Sleep -Seconds 6

# drive the GUI: paste wiki text, save
$shell = New-Object -ComObject WScript.Shell
$p = Get-Process MDLite-dbg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p) {
    $shell.AppActivate($p.Id) | Out-Null
    Start-Sleep -Milliseconds 800
    Set-Clipboard -Value "link [[B]] and [[C]] now"
    [System.Windows.Forms.SendKeys]::SendWait("^a")
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait("^v")
    Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait("^{s}")
    Start-Sleep -Milliseconds 800
    [System.Windows.Forms.SendKeys]::SendWait("^a")
    [System.Windows.Forms.SendKeys]::SendWait("^v")
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait("^{s}")
    Start-Sleep -Seconds 5
    if (-not $gdb.HasExited) { "target alive after repro; stopping gdb" ; $gdb.Kill() }
} else { "target died before automation - crash reproduced" }

Start-Sleep -Seconds 1
"=== gdb out (tail) ==="
Get-Content "$tdir\gdb_out.log" -Tail 50
