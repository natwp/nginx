class TestRcWebClient {
    static [int] $DisposeCount = 0
    [object] $Proxy

    [string] DownloadString([string] $Url) {
        return 'expected'
    }

    [void] Dispose() {
        [TestRcWebClient]::DisposeCount++
    }
}

. "$PSScriptRoot\win32-rc-test.ps1" -Library

[TestRcWebClient]::DisposeCount = 0
$factory = { return [TestRcWebClient]::new() }
Wait-Http 1 'expected' 1 $factory

if ([TestRcWebClient]::DisposeCount -ne 1) {
    throw "Wait-Http disposed $([TestRcWebClient]::DisposeCount) clients, expected 1"
}

Write-Host 'Wait-Http disposal test passed'

$reader = [IO.StringReader]::new(
    "Date: Sat, 19 Sep 2026 12:34:56 GMT`r`n" +
    "Location: http://127.0.0.1:8080/path`r`n`r`n")
try {
    $headers = Read-HttpHeaders $reader
    Assert-True ($headers['Date'] -eq 'Sat, 19 Sep 2026 12:34:56 GMT') `
        'Header parsing changed the date value'
    Assert-True ($headers['Location'] -eq 'http://127.0.0.1:8080/path') `
        'Header parsing changed the location value'
} finally {
    $reader.Dispose()
}

Write-Host 'HTTP header parsing test passed'
