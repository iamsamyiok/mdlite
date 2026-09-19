# reproduce the [[ crash under gdb + PageHeap
$tdir = "E:\Desktop\mdlite-src\testnotes"
Set-Location $tdir

# launch under gdb in background
$gdb = Start-Process -FilePath "E:\download6\winlibs\mingw64\bin\gdb.exe" `
    -ArgumentList "-q","-batch","-ex","set pagination off","-ex","run","-ex","bt","-ex","info registers eip rip","-ex","quit","--args","$tdir\MDLite-dbg.exe","$tdir\A.md" `
    -RedirectStandardOutput "$tdir\gdb_out.log" -RedirectStandardError "$tdir\gdb_err.log" `
    -PassThru
Start-Sleep -Seconds 6

# bring the editor to front and drive it
$shell = New-Object -ComObject WScript.Shell
$shell.AppActivate((Get-Process MDLite-dbg | Select-Object -First 1).Id) | Out-Null
Start-Sleep -Milliseconds 800
$shell.SendKeys("{END}")
Start-Sleep -Milliseconds 300
$shell.SendKeys("see [[B]] now ")
Start-Sleep -Milliseconds 600
# the wiki popup should be up; pick the entry with keyboard
$shell.SendKeys("{ESC}")   # close popup if still up
Start-Sleep -Milliseconds 300
$shell.SendKeys("^{s}")    # save -> triggers backlinks sync
Start-Sleep -Seconds 4

if ($gdb.HasExited) { "gdb already exited" } else { "gdb still running; stopping target" }
# if the target crashed under gdb, gdb printed the backtrace; give it a moment then kill
Start-Sleep -Seconds 2
if (-not $gdb.HasExited) { $gdb.Kill() }
"gdb output:"
Get-Content "$tdir\gdb_out.log" -Tail 40
