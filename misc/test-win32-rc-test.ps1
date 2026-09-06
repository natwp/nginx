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
