# 1) enable PageHeap for the debug exe (catch heap OOB at the exact write)
$exe = "MDLite-dbg.exe"
$ifeo = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$exe"
New-Item -Path $ifeo -Force | Out-Null
Set-ItemProperty -Path $ifeo -Name GlobalFlag -Value 0x02000000
Set-ItemProperty -Path $ifeo -Name PageHeapFlags -Value 0x3
"PageHeap enabled for $exe"

# 2) prepare isolated test dir
$tdir = "E:\Desktop\mdlite-src\testnotes"
New-Item -ItemType Directory -Force -Path $tdir | Out-Null
Set-Content -Path "$tdir\A.md" -Value "# A`n`nsee [[B]] here`n" -Encoding UTF8
Set-Content -Path "$tdir\B.md" -Value "# B`n`ncontent of B`n" -Encoding UTF8
Copy-Item "E:\Desktop\mdlite-src\mdlite\MDLite-dbg.exe" "$tdir\MDLite-dbg.exe" -Force
"test dir ready: $tdir"
