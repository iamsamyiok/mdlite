$e = Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'} -MaxEvents 3 |
    Where-Object { $_.Message -match 'MDLite-dbg' } | Select-Object -First 1
if ($e) {
    $e.TimeCreated
    ($e.Message -split "`n" | Where-Object { $_ -match '模块名称|异常代码|偏移量' })
} else { "no crash event yet" }
