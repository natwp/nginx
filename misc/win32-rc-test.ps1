<#
.SYNOPSIS
    Runs the native Windows release-candidate smoke and lifecycle matrix.

.DESCRIPTION
    Uses only loopback sockets and a unique temporary prefix. It exercises
    IOCP, select, and poll with two workers, then checks reload, reopen, worker
    replacement, idle listeners, file paths, PCRE2, gzip, optional TLS and
    mail, and stderr logging.
#>

[CmdletBinding()]
param(
    [string] $Binary = (Join-Path $PSScriptRoot '..\objs\nginx.exe'),
    [string] $Root = (Join-Path $PSScriptRoot '..'),
    [string] $OpenSSLBinary = '',
    [switch] $TestMail,
    [switch] $KeepArtifacts,
    [switch] $Library
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Get-FreeTcpPort {
    $listener = New-Object -TypeName System.Net.Sockets.TcpListener -ArgumentList ([IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

function Get-FreeUdpPort {
    $client = New-Object -TypeName System.Net.Sockets.UdpClient -ArgumentList 0
    $port = $client.Client.LocalEndPoint.Port
    $client.Close()
    return $port
}

function Invoke-Nginx([string[]] $Arguments) {
    $savedErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $script:Binary @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($exitCode -ne 0) {
        throw "nginx command failed ($exitCode): $($Arguments -join ' ')`n$($output -join "`n")"
    }
}

function Invoke-NginxEmptyPrefix([string] $Config) {
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $script:Binary
    $startInfo.Arguments = "-e stderr -p `"`" -c $Config -t"
    $startInfo.WorkingDirectory = $script:Root
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void] $process.Start()
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    if ($process.ExitCode -ne 0) {
        throw "empty-prefix nginx command failed ($($process.ExitCode))`n$stdout$stderr"
    }
}

function Invoke-OpenSSL([string[]] $Arguments) {
    $savedErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $script:OpenSSL @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($exitCode -ne 0) {
        throw "OpenSSL command failed ($exitCode): $($Arguments -join ' ')`n$($output -join "`n")"
    }
}

function Get-TestProcesses([string] $Prefix) {
    $needle = [IO.Path]::GetFullPath($Prefix).TrimEnd('\')
    try {
        return @(Get-CimInstance Win32_Process -Filter "Name='nginx.exe'" |
            Where-Object {
                $_.CommandLine -and $_.CommandLine.IndexOf(
                    $needle, [StringComparison]::OrdinalIgnoreCase) -ge 0
            })
    } catch {
        return @()
    }
}

function Wait-Http([int] $Port, [string] $Expected, [int] $TimeoutSec = 20,
    [scriptblock] $ClientFactory = $null) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $client = $null
        try {
            $client = if ($ClientFactory) {
                & $ClientFactory
            } else {
                New-Object System.Net.WebClient
            }
            $client.Proxy = $null
            $body = $client.DownloadString("http://127.0.0.1:$Port/")
            if ($body -eq $Expected) {
                return
            }
        } catch {
            Start-Sleep -Milliseconds 200
        } finally {
            if ($client) { $client.Dispose() }
        }
    } while ((Get-Date) -lt $deadline)
    throw "HTTP backend on port $Port did not return '$Expected'"
}

function Wait-TestProcesses([string] $Prefix, [int] $Minimum, [int] $TimeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $processes = @(Get-TestProcesses $Prefix)
        if ($processes.Count -ge $Minimum) {
            return $processes
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    throw "expected at least $Minimum nginx processes for $Prefix"
}

function Wait-Workers([string] $Prefix, [int] $MasterId, [int] $Expected,
    [int] $ExcludedId = 0, [int] $TimeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $workers = @(Get-TestProcesses $Prefix |
            Where-Object { $_.ParentProcessId -eq $MasterId })
        $excluded = @($workers | Where-Object { $_.ProcessId -eq $ExcludedId })
        if ($workers.Count -eq $Expected -and $excluded.Count -eq 0) {
            return $workers
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    throw "expected $Expected workers for master $MasterId"
}

function Wait-Https([int] $Port, [string] $Expected, [int] $TimeoutSec = 30) {
    Write-Host "  Attempting HTTPS connection to https://127.0.0.1:$Port/" -ForegroundColor Yellow

    # Try curl first if available (more reliable with self-signed certs)
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        Write-Host "  Using curl for HTTPS test" -ForegroundColor Yellow
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        do {
            try {
                $body = & curl.exe -k -s -m 2 "https://127.0.0.1:$Port/"
                if ($LASTEXITCODE -eq 0 -and $body -eq $Expected) {
                    Write-Host "  HTTPS response received via curl: '$body'" -ForegroundColor Green
                    return
                }
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "  curl failed with exit code $LASTEXITCODE" -ForegroundColor DarkYellow
                } elseif ($body -ne $Expected) {
                    Write-Host "  Response mismatch: got '$body', expected '$Expected'" -ForegroundColor Red
                }
            } catch {
                Write-Host "  curl exception: $($_.Exception.Message)" -ForegroundColor DarkYellow
            }
            Start-Sleep -Milliseconds 500
        } while ((Get-Date) -lt $deadline)
        throw "HTTPS backend on port $Port did not return '$Expected' (curl method)"
    }

    # Fallback to WebClient
    $oldCallback = [Net.ServicePointManager]::ServerCertificateValidationCallback
    $oldProtocol = [Net.ServicePointManager]::SecurityProtocol
    $lastError = $null
    try {
        [Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls11 -bor [Net.SecurityProtocolType]::Tls
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        do {
            try {
                $client = New-Object System.Net.WebClient
                $client.Proxy = $null
                $body = $client.DownloadString("https://127.0.0.1:$Port/")
                if ($body -eq $Expected) {
                    Write-Host "  HTTPS response received: '$body'" -ForegroundColor Green
                    return
                }
                Write-Host "  Response mismatch: got '$body', expected '$Expected'" -ForegroundColor Red
            } catch {
                $lastError = $_
                $msg = $_.Exception.Message
                if ($_.Exception.InnerException) {
                    $msg += " (Inner: $($_.Exception.InnerException.Message))"
                }
                Write-Host "  HTTPS attempt failed: $msg" -ForegroundColor DarkYellow
                Start-Sleep -Milliseconds 500
            } finally {
                if ($client) { $client.Dispose() }
            }
        } while ((Get-Date) -lt $deadline)
        Write-Host "  HTTPS connection timed out after $TimeoutSec seconds" -ForegroundColor Red
        $errorMsg = "HTTPS backend on port $Port did not return '$Expected'"
        if ($lastError) {
            $errorMsg += ". Last error: $($lastError.Exception.Message)"
            if ($lastError.Exception.InnerException) {
                $errorMsg += " (Inner: $($lastError.Exception.InnerException.Message))"
            }
        }
        throw $errorMsg
    } finally {
        [Net.ServicePointManager]::ServerCertificateValidationCallback = $oldCallback
        [Net.ServicePointManager]::SecurityProtocol = $oldProtocol
    }
}

function Read-HttpHeaders([IO.TextReader] $Reader) {
    $headers = @{}
    for ($i = 0; $i -lt 100; $i++) {
        $line = $Reader.ReadLine()
        if ($null -eq $line) { throw 'HTTP response ended before headers' }
        if ($line -eq '') { return $headers }
        $fields = $line.Split([char[]] ':', 2)
        Assert-True ($fields.Length -eq 2) "Invalid HTTP header: $line"
        $headers[$fields[0]] = $fields[1].Trim()
    }
    throw 'Too many HTTP response headers'
}

function Test-ExpectContinue([int] $Port, [string] $Expected, [switch] $Tls) {
    $tcp = New-Object -TypeName System.Net.Sockets.TcpClient
    $ssl = $null
    $reader = $null

    try {
        $tcp.ReceiveTimeout = 5000
        $tcp.SendTimeout = 5000
        $tcp.Connect([IPAddress]::Loopback, $Port)
        $stream = $tcp.GetStream()
        if ($Tls) {
            $callback = [Net.Security.RemoteCertificateValidationCallback] {
                param($sender, $certificate, $chain, $errors)
                return $true
            }
            $ssl = [Net.Security.SslStream]::new($stream, $false, $callback)
            $ssl.AuthenticateAsClient('localhost', $null,
                [Security.Authentication.SslProtocols]::Tls12, $false)
            $stream = $ssl
        }
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::ASCII, $false, 4096, $true)

        $body = 'expect-body'
        $headers = "PUT /expect HTTP/1.1`r`nHost: localhost`r`n" +
                   "Content-Length: $($body.Length)`r`n" +
                   "Expect: 100-continue`r`nConnection: close`r`n`r`n"
        $headerBytes = [Text.Encoding]::ASCII.GetBytes($headers)
        $stream.Write($headerBytes, 0, $headerBytes.Length)
        $stream.Flush()

        $status = $reader.ReadLine()
        Assert-True ($status -eq 'HTTP/1.1 100 Continue') `
            "Expect request (TLS=$Tls) returned '$status' instead of 100 Continue"
        [void] (Read-HttpHeaders $reader)

        $bodyBytes = [Text.Encoding]::ASCII.GetBytes($body)
        $stream.Write($bodyBytes, 0, $bodyBytes.Length)
        $stream.Flush()

        $status = $reader.ReadLine()
        Assert-True ($status -eq 'HTTP/1.1 200 OK') `
            "Expect request (TLS=$Tls) returned '$status' instead of 200 OK"
        [void] (Read-HttpHeaders $reader)
        $response = $reader.ReadToEnd()
        Assert-True ($response -eq $Expected) `
            "Expect response body mismatch: '$response'"
    } finally {
        if ($reader) { $reader.Dispose() }
        if ($ssl) { $ssl.Dispose() }
        if ($tcp) { $tcp.Close() }
    }
}

function Open-TlsTestClient([int] $Port) {
    $tcp = [Net.Sockets.TcpClient]::new()
    try {
        $tcp.ReceiveBufferSize = 16384
        $tcp.ReceiveTimeout = 10000
        $tcp.SendTimeout = 10000
        $tcp.Connect([IPAddress]::Loopback, $Port)
        $callback = [Net.Security.RemoteCertificateValidationCallback] {
            param($sender, $certificate, $chain, $errors)
            return $true
        }
        $ssl = [Net.Security.SslStream]::new($tcp.GetStream(), $false, $callback)
        $ssl.ReadTimeout = 10000
        $ssl.WriteTimeout = 10000
        $ssl.AuthenticateAsClient('localhost', $null,
            [Security.Authentication.SslProtocols]::Tls12, $false)
        return @{ Tcp = $tcp; Stream = $ssl }
    } catch {
        $tcp.Dispose()
        throw
    }
}

function Start-TlsFileResponse($Client, [int] $Length) {
    $request = [Text.Encoding]::ASCII.GetBytes(
        "GET /backpressure.bin HTTP/1.1`r`nHost: localhost`r`n`r`n")
    $Client.Stream.Write($request, 0, $request.Length)
    $Client.Stream.Flush()

    # Read only headers: StreamReader can consume binary body bytes which
    # the subsequent raw reads would otherwise lose.
    $bytes = [Collections.Generic.List[byte]]::new()
    do {
        $value = $Client.Stream.ReadByte()
        Assert-True ($value -ge 0) 'TLS response ended before headers'
        $bytes.Add([byte] $value)
        $n = $bytes.Count
        Assert-True ($n -le 16384) 'TLS response headers exceed the test limit'
    } until ($n -ge 4 -and $bytes[$n - 4] -eq 13 -and $bytes[$n - 3] -eq 10 `
        -and $bytes[$n - 2] -eq 13 -and $bytes[$n - 1] -eq 10)

    $headers = [Text.Encoding]::ASCII.GetString($bytes.ToArray())
    Assert-True ($headers.StartsWith("HTTP/1.1 200 OK`r`n")) 'TLS file status is not 200'
    Assert-True ($headers -match "(?im)^Content-Length: $Length\r$") `
        'TLS file length is incorrect'
    Assert-True ($headers -notmatch '(?im)^Transfer-Encoding:') `
        'TLS file unexpectedly uses transfer encoding'
    Assert-True ($headers -match '(?im)^X-RC-Worker: (\d+)\r$') `
        'TLS response is missing its worker identity'
    return [int] $Matches[1]
}

function Test-TlsBackpressure([int] $Port, [byte[]] $Expected,
    [scriptblock] $WhileBlocked) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $expectedHash = [Convert]::ToBase64String($sha.ComputeHash($Expected))
    $client = $null
    try {
        $client = Open-TlsTestClient $Port
        $firstWorker = 0
        for ($request = 0; $request -lt 3; $request++) {
            $worker = Start-TlsFileResponse $client $Expected.Length
            if ($request -eq 0) { $firstWorker = $worker }
            Assert-True ($worker -eq $firstWorker) 'TLS keepalive changed workers'
            Start-Sleep -Milliseconds 150
            if ($request -eq 2) { & $WhileBlocked $worker }

            $response = [byte[]]::new($Expected.Length)
            $received = 0
            $nextPause = 262144
            while ($received -lt $response.Length) {
                $count = $client.Stream.Read($response, $received,
                    [Math]::Min(65536, $response.Length - $received))
                Assert-True ($count -gt 0) 'TLS file body was truncated'
                $received += $count
                if ($received -ge $nextPause) {
                    Start-Sleep -Milliseconds 10
                    $nextPause += 262144
                }
            }
            $actualHash = [Convert]::ToBase64String($sha.ComputeHash($response))
            Assert-True ($actualHash -eq $expectedHash) 'TLS file body was corrupted'
        }
        $client.Stream.Dispose()
        $client.Tcp.Dispose()
        $client = Open-TlsTestClient $Port
        [void] (Start-TlsFileResponse $client $Expected.Length)
        Start-Sleep -Milliseconds 150
        # Reset with the server still blocked. No TLS close-notify is sent.
        $client.Tcp.Client.LingerState = [Net.Sockets.LingerOption]::new($true, 0)
        $client.Tcp.Dispose()
    } finally {
        if ($client) {
            try { $client.Stream.Dispose() } finally { $client.Tcp.Dispose() }
        }
        $sha.Dispose()
    }
}

function Test-ReadNotifications([int] $Port, [switch] $Tls) {
    $client = $null
    $reader = $null
    try {
        if ($Tls) {
            $client = Open-TlsTestClient $Port
        } else {
            $tcp = [Net.Sockets.TcpClient]::new()
            $client = @{ Tcp = $tcp; Stream = $null }
            $tcp.ReceiveTimeout = 5000
            $tcp.SendTimeout = 5000
            $tcp.Connect([IPAddress]::Loopback, $Port)
            $client.Stream = $tcp.GetStream()
        }
        $reader = [IO.StreamReader]::new(
            $client.Stream, [Text.Encoding]::ASCII, $false, 4096, $true)
        $workerId = $null
        for ($round = 0; $round -lt 16; $round++) {
            # Alternate fragmented requests with pipelines on one connection.
            # Both paths can rearm a read from inside its completion handler.
            $count = if ($round % 2) { 4 } else { 1 }
            $padding = 'p' * $(if ($round % 4 -eq 0) { 6144 } else { 17 })
            $requests = ''
            for ($index = 0; $index -lt $count; $index++) {
                $requests += "GET /read-notify?value=r$round-i$index HTTP/1.1`r`n" +
                    "Host: localhost`r`nX-Read-Padding: $padding`r`n`r`n"
            }
            $bytes = [Text.Encoding]::ASCII.GetBytes($requests)
            if ($count -eq 1) {
                $client.Stream.Write($bytes, 0, $bytes.Length - 2)
                $client.Stream.Flush()
                Start-Sleep -Milliseconds 10
                $client.Stream.Write($bytes, $bytes.Length - 2, 2)
            } else {
                $client.Stream.Write($bytes, 0, $bytes.Length)
            }
            $client.Stream.Flush()
            for ($index = 0; $index -lt $count; $index++) {
                Assert-True ($reader.ReadLine() -eq 'HTTP/1.1 200 OK') `
                    'Read notification lost an HTTP response'
                $headers = Read-HttpHeaders $reader
                $expected = "r$round-i$index"
                Assert-True ([int] $headers['Content-Length'] -eq $expected.Length) `
                    'Read notification changed the response length'
                $chars = [char[]]::new($expected.Length)
                Assert-True ($reader.ReadBlock($chars, 0, $chars.Length) -eq $chars.Length) `
                    'Read notification truncated a response'
                Assert-True ((-join $chars) -eq $expected) `
                    'Read notification reordered or duplicated response data'
                $identity = $headers['X-RC-Worker']
                Assert-True ($identity -match '^\d+$') 'Missing read test worker identity'
                if ($workerId) {
                    Assert-True ($identity -eq $workerId) 'Read test changed workers'
                }
                $workerId = $identity
            }
            Start-Sleep -Milliseconds 10
        }
        # Leave a fragmented request pending, then reset the connection to
        # exercise cancellation after repeated operation reuse.
        $partial = [Text.Encoding]::ASCII.GetBytes('GET /read-notify?value=unfinished')
        $client.Stream.Write($partial, 0, $partial.Length)
        $client.Stream.Flush()
        Start-Sleep -Milliseconds 20
        $client.Tcp.Client.LingerState = [Net.Sockets.LingerOption]::new($true, 0)
        $client.Tcp.Dispose()
        Write-Host "PASS fragmented/pipelined requests and pending read reset (TLS=$Tls)"
    } finally {
        if ($reader) { $reader.Dispose() }
        if ($client) {
            try {
                if ($client.Stream) { $client.Stream.Dispose() }
            } finally {
                $client.Tcp.Dispose()
            }
        }
    }
}

function Test-TlsHandshakeReset([int] $Port) {
    # A TLS record header and a partial ClientHello leave an encrypted
    # receive pending when SSL and connection state are destroyed.
    $partial = [byte[]] (0x16, 0x03, 0x01, 0x00, 0x40,
                         0x01, 0x00, 0x00, 0x3c, 0x03, 0x03)
    for ($round = 0; $round -lt 8; $round++) {
        $tcp = [Net.Sockets.TcpClient]::new()
        try {
            $tcp.Connect([IPAddress]::Loopback, $Port)
            $stream = $tcp.GetStream()
            $stream.Write($partial, 0, 5)
            $stream.Flush()
            Start-Sleep -Milliseconds 10
            $stream.Write($partial, 5, $partial.Length - 5)
            $stream.Flush()
            Start-Sleep -Milliseconds 20
            $tcp.Client.LingerState = [Net.Sockets.LingerOption]::new($true, 0)
        } finally {
            $tcp.Dispose()
        }
    }
    Write-Host 'PASS reset during fragmented TLS handshakes'
}

function Test-TlsBufferedReads([int] $Port) {
    $client = Open-TlsTestClient $Port
    $reader = $null
    try {
        $reader = [IO.StreamReader]::new(
            $client.Stream, [Text.Encoding]::ASCII, $false, 4096, $true)
        $lengths = @(512, 8192, 1, 1536)
        for ($round = 0; $round -lt $lengths.Length; $round++) {
            $expected = ('body' + $round + '-') * $lengths[$round]
            $body = [Text.Encoding]::ASCII.GetBytes($expected)
            $headers = [Text.Encoding]::ASCII.GetBytes(
                "POST /tls-upload HTTP/1.1`r`nHost: localhost`r`n" +
                "Content-Length: $($body.Length)`r`n`r`n")
            $client.Stream.Write($headers, 0, $headers.Length)
            $client.Stream.Flush()
            Start-Sleep -Milliseconds 20
            for ($offset = 0; $offset -lt $body.Length; $offset += 251) {
                $length = [Math]::Min(251, $body.Length - $offset)
                $client.Stream.Write($body, $offset, $length)
                $client.Stream.Flush()
                Start-Sleep -Milliseconds 1
            }
            Assert-True ($reader.ReadLine() -eq 'HTTP/1.1 200 OK') `
                'TLS upload or TLS upstream failed'
            $responseHeaders = Read-HttpHeaders $reader
            Assert-True ([int] $responseHeaders['Content-Length'] -eq $expected.Length) `
                'TLS upload response length changed'
            $response = [char[]]::new($expected.Length)
            Assert-True ($reader.ReadBlock($response, 0, $response.Length) -eq $response.Length) `
                'TLS upload response was truncated'
            Assert-True ((-join $response) -eq $expected) `
                'TLS upload or TLS upstream changed the body'
        }
    } finally {
        if ($reader) { $reader.Dispose() }
        $client.Stream.Dispose()
        $client.Tcp.Dispose()
    }

    $client = Open-TlsTestClient $Port
    try {
        $partial = [Text.Encoding]::ASCII.GetBytes(
            "POST /tls-upload HTTP/1.1`r`nHost: localhost`r`n" +
            "Content-Length: 16384`r`n`r`npartial-body")
        $client.Stream.Write($partial, 0, $partial.Length)
        $client.Stream.Flush()
        Start-Sleep -Milliseconds 30
        $client.Tcp.Client.LingerState = [Net.Sockets.LingerOption]::new($true, 0)
        $client.Tcp.Dispose()
    } finally {
        $client.Stream.Dispose()
        $client.Tcp.Dispose()
    }
    Write-Host 'PASS fragmented TLS uploads, TLS upstream, and pending body reset'
}

function Get-HttpBody([int] $Port, [string] $Path, [int] $TimeoutSec = 20) {
    # Retry transient connection failures.  A single reset on a loopback
    # request should not fail the run, and these fetches happen right after a
    # worker respawn, when a connection can still be torn down mid-flight.
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastError = $null

    do {
        $client = $null
        try {
            $client = New-Object System.Net.WebClient
            $client.Proxy = $null
            return $client.DownloadString("http://127.0.0.1:$Port$Path")
        } catch {
            $lastError = $_
            Start-Sleep -Milliseconds 300
        } finally {
            if ($client) { $client.Dispose() }
        }
    } while ((Get-Date) -lt $deadline)

    $message = "GET $Path on port $Port failed"
    if ($lastError) {
        $message += ": $($lastError.Exception.Message)"
        if ($lastError.Exception.InnerException) {
            $message += " (inner: $($lastError.Exception.InnerException.Message))"
        }
    }
    throw $message
}

function Test-Gzip([int] $Port, [string] $Expected, [int] $TimeoutSec = 20) {
    # Retries transport failures for the same reason as Get-HttpBody, while
    # still failing immediately on a wrong or uncompressed response.
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastError = $null

    do {
        $response = $null
        $gzip = $null
        $reader = $null
        try {
            $request = [Net.HttpWebRequest]::Create("http://127.0.0.1:$Port/gzip.txt")
            $request.Proxy = $null
            $request.Headers.Add('Accept-Encoding', 'gzip')
            $response = [Net.HttpWebResponse] $request.GetResponse()
            Assert-True ($response.Headers['Content-Encoding'] -eq 'gzip') 'gzip response was not compressed'
            $gzip = New-Object IO.Compression.GZipStream -ArgumentList ($response.GetResponseStream(), [IO.Compression.CompressionMode]::Decompress)
            $reader = New-Object IO.StreamReader -ArgumentList $gzip
            $body = $reader.ReadToEnd()
            Assert-True ($body -eq $Expected) 'gzip response body mismatch'
            return
        } catch [Net.WebException] {
            $lastError = $_
            Start-Sleep -Milliseconds 300
        } finally {
            if ($reader) { $reader.Dispose() }
            elseif ($gzip) { $gzip.Dispose() }
            if ($response) { $response.Dispose() }
        }
    } while ((Get-Date) -lt $deadline)

    $message = "gzip request on port $Port failed"
    if ($lastError) { $message += ": $($lastError.Exception.Message)" }
    throw $message
}

function Invoke-Udp([int] $Port, [string] $Expected,
    [System.Net.Sockets.UdpClient] $Client = $null) {
    $owned = $false
    if (-not $Client) {
        $Client = New-Object -TypeName System.Net.Sockets.UdpClient
        $owned = $true
    }

    try {
        $Client.Client.ReceiveTimeout = 5000
        $remote = New-Object -TypeName System.Net.IPEndPoint -ArgumentList ([IPAddress]::Loopback, $Port)
        $payload = [Text.Encoding]::ASCII.GetBytes('win32-rc')
        [void] $Client.Send($payload, $payload.Length, $remote)
        $source = New-Object -TypeName System.Net.IPEndPoint -ArgumentList ([IPAddress]::Any, 0)
        $reply = $Client.Receive([ref] $source)
        $text = [Text.Encoding]::ASCII.GetString($reply)
        Assert-True ($text -eq $Expected) "UDP returned '$text', expected '$Expected'"
    } finally {
        if ($owned) { $Client.Close() }
    }
}

function Test-MailFirstCommands([int] $Port, [string] $Protocol) {
    $client = New-Object -TypeName System.Net.Sockets.TcpClient
    $reader = $null
    try {
        $client.ReceiveTimeout = 5000
        $client.SendTimeout = 5000
        $client.Connect([IPAddress]::Loopback, $Port)
        $stream = $client.GetStream()
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::ASCII, $false, 4096, $true)
        $greeting = $reader.ReadLine()
        $pattern = if ($Protocol -eq 'imap') { '^\* OK(?: |$)' } else { '^\+OK(?: |$)' }
        Assert-True ($greeting -match $pattern) "$Protocol greeting failed: $greeting"

        foreach ($number in 1..2) {
            $command = if ($Protocol -eq 'imap') {
                "a$number NOOP`r`n"
            } else {
                "NOOP`r`n"
            }
            $bytes = [Text.Encoding]::ASCII.GetBytes($command)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            $reply = $reader.ReadLine()
            $pattern = if ($Protocol -eq 'imap') {
                "^a$number OK(?: |`$)"
            } else {
                '^\+OK(?: |$)'
            }
            Assert-True ($reply -match $pattern) "$Protocol command $number failed: $reply"
        }
        Write-Host "PASS $Protocol greeting and first/subsequent commands"
    } finally {
        if ($reader) { $reader.Dispose() }
        $client.Close()
    }
}

function Start-PartialHttpRequest([int] $Port) {
    $client = New-Object -TypeName System.Net.Sockets.TcpClient
    $reader = $null
    try {
        $client.ReceiveTimeout = 5000
        $client.SendTimeout = 5000
        $client.Connect([IPAddress]::Loopback, $Port)
        $stream = $client.GetStream()
        $request = [Text.Encoding]::ASCII.GetBytes(
            "GET /worker HTTP/1.1`r`nHost: localhost`r`n`r`n")
        $stream.Write($request, 0, $request.Length)
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::ASCII, $false, 4096, $true)
        Assert-True ($reader.ReadLine() -eq 'HTTP/1.1 200 OK') 'Worker probe failed'
        $headers = Read-HttpHeaders $reader
        $length = [int] $headers['Content-Length']
        Assert-True ($length -gt 0 -and $length -le 16) 'Invalid worker ID length'
        $chars = New-Object char[] $length
        Assert-True ($reader.ReadBlock($chars, 0, $length) -eq $length) 'Missing worker ID'
        $workerId = [int] (-join $chars)
        $partial = [Text.Encoding]::ASCII.GetBytes(
            "GET / HTTP/1.1`r`nHost: 127.0.0.1`r`n")
        $stream.Write($partial, 0, $partial.Length)
        $stream.Flush()
        return @{Client = $client; WorkerId = $workerId}
    } catch {
        $client.Close()
        throw
    } finally {
        if ($reader) { $reader.Dispose() }
    }
}

function Test-IdleListeners([int] $FirstPort, [int] $SecondPort) {
    $connections = @()
    try {
        # Alternate connection bursts between listeners, keeping earlier
        # connections open. Stale accept events must not block the workers
        # responsible for those established connections.
        $request = [Text.Encoding]::ASCII.GetBytes(
            "GET /worker HTTP/1.1`r`nHost: localhost`r`nConnection: keep-alive`r`n`r`n")
        for ($round = 0; $round -lt 4; $round++) {
            $port = if ($round % 2) { $SecondPort } else { $FirstPort }
            $added = @()
            for ($i = 0; $i -lt 8; $i++) {
                $client = New-Object -TypeName System.Net.Sockets.TcpClient
                $client.ReceiveTimeout = 5000
                $client.SendTimeout = 5000
                $entry = @{Client = $client; Pending = $null; Reader = $null}
                $connections += $entry
                $added += $entry
                $entry.Pending = $client.BeginConnect([IPAddress]::Loopback,
                    $port, $null, $null)
            }
            foreach ($entry in $added) {
                Assert-True ($entry.Pending.AsyncWaitHandle.WaitOne(5000)) `
                    'Listener connection timed out'
                $entry.Client.EndConnect($entry.Pending)
                $entry.Stream = $entry.Client.GetStream()
                $entry.Reader = [IO.StreamReader]::new(
                    $entry.Stream, [Text.Encoding]::ASCII, $false, 4096, $true)
            }
            Start-Sleep -Milliseconds 100
            foreach ($entry in $connections) {
                $entry.Stream.Write($request, 0, $request.Length)
            }
            foreach ($entry in $connections) {
                Assert-True ($entry.Reader.ReadLine() -eq 'HTTP/1.1 200 OK') `
                    'Idle listener blocked an established connection'
                $headers = Read-HttpHeaders $entry.Reader
                $length = [int] $headers['Content-Length']
                Assert-True ($length -gt 0 -and $length -le 16) `
                    'Invalid worker response length'
                $chars = New-Object char[] $length
                Assert-True ($entry.Reader.ReadBlock($chars, 0, $length) -eq $length) `
                    'Incomplete worker response'
                $workerId = -join $chars
                Assert-True ($workerId -match '^\d+$') 'Invalid worker ID'
                if ($entry.ContainsKey('WorkerId')) {
                    Assert-True ($workerId -eq $entry.WorkerId) `
                        'Keepalive connection changed worker'
                }
                $entry.WorkerId = $workerId
            }
        }
        Write-Host 'PASS established connections with two idle listeners'
    } finally {
        foreach ($entry in $connections) {
            if ($entry.Reader) { $entry.Reader.Dispose() }
            $entry.Client.Close()
            if ($entry.Pending) { $entry.Pending.AsyncWaitHandle.Close() }
        }
    }
}

function Stop-TestNginx([string] $Prefix, [string] $Config, [System.Diagnostics.Process] $Master) {
    try {
        & $script:Binary '-e' 'stderr' '-p' $Prefix '-c' $Config '-s' 'quit' 2>&1 | Out-Null
    } catch {
    }
    if ($Master) {
        if (-not $Master.WaitForExit(10000)) {
            Stop-Process -Id $Master.Id -Force -ErrorAction SilentlyContinue
            [void] $Master.WaitForExit(5000)
        }
    }

    $deadline = (Get-Date).AddSeconds(10)
    do {
        $remaining = @(Get-TestProcesses $Prefix)
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)

    $remaining | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
    throw "nginx processes remained after quit for $Prefix"
}

function Test-Backend([string] $Backend, [string] $BaseDir) {
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Testing backend: $Backend" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan

    $dir = Join-Path $BaseDir $Backend
    $logs = Join-Path $dir 'logs'
    $html = Join-Path $dir 'html'
    New-Item -ItemType Directory -Force -Path $logs, $html | Out-Null

    $httpPort = Get-FreeTcpPort
    $secondHttpPort = Get-FreeTcpPort
    $httpsPort = Get-FreeTcpPort
    $udpPort = Get-FreeUdpPort
    $imapPort = if ($TestMail) { Get-FreeTcpPort } else { 0 }
    $pop3Port = if ($TestMail) { Get-FreeTcpPort } else { 0 }

    Write-Host "Ports: HTTP=$httpPort, HTTPS=$httpsPort, UDP=$udpPort" -ForegroundColor Yellow
    $config = Join-Path $dir 'nginx.conf'
    $pidFile = Join-Path $logs 'nginx.pid'
    $stderr = Join-Path $logs 'stderr.log'
    $body = "backend=$Backend"
    $reloadedBody = "$body-reloaded"
    $udpBody = "udp=$Backend"
    $fileBody = ('file-aio-sendfile-' * 4096)
    $aioLocation = if ($Backend -eq 'iocp') {
@"
        location = /aio.txt {
            root "$($html.Replace('\','/'))";
            sendfile off;
            aio on;
        }
"@
    } else { '' }
    $streamConfig = if ($Backend -eq 'iocp') {
@"
stream {
    log_format rc_stream '`$remote_addr [`$time_local] `$protocol `$status';
    access_log stderr rc_stream;
    server {
        listen 127.0.0.1:$udpPort udp;
        return "$udpBody";
    }
}
"@
    } else { '' }
    $mailConfig = if ($TestMail) {
@"
mail {
    server_name localhost;
    auth_http http://127.0.0.1:$httpPort/auth;
    timeout 10s;
    server {
        listen 127.0.0.1:$imapPort;
        protocol imap;
    }
    server {
        listen 127.0.0.1:$pop3Port;
        protocol pop3;
    }
}
"@
    } else { '' }
    $tlsConfig = if ($script:Tls) {
@"
        listen 127.0.0.1:$httpsPort ssl sndbuf=16k;
        ssl_certificate "$($script:Tls.Cert.Replace('\','/'))";
        ssl_certificate_key "$($script:Tls.Key.Replace('\','/'))";
        large_client_header_buffers 4 64k;
        location = /tls-upload {
            client_body_buffer_size 64k;
            proxy_pass https://127.0.0.1:$httpsPort/tls-echo;
            proxy_set_header X-RC-Body `$request_body;
        }
        location = /tls-echo {
            return 200 "`$http_x_rc_body";
        }
"@
    } else { '' }
    foreach ($file in @('aio.txt', 'gzip.txt', 'sendfile.txt')) {
        Set-Content -LiteralPath (Join-Path $html $file) -Value $fileBody `
            -NoNewline -Encoding ASCII
    }
    $tlsBody = $null
    if ($script:Tls) {
        $tlsBody = [byte[]]::new(4 * 1024 * 1024)
        $block = [byte[]] (0..255)
        for ($offset = 0; $offset -lt $tlsBody.Length; $offset += $block.Length) {
            [Array]::Copy($block, 0, $tlsBody, $offset, $block.Length)
        }
        [IO.File]::WriteAllBytes((Join-Path $html 'backpressure.bin'), $tlsBody)
    }

    $conf = @"
worker_processes 2;
error_log stderr notice;
pid "$($pidFile.Replace('\','/'))";

events {
    use $Backend;
    worker_connections 512;
    iocp_threads 1;
    post_acceptex 8;
    iocp_udp_receives 8;
}

http {
    access_log stderr;
    log_format rc_tls_bytes '`$status `$body_bytes_sent';
    default_type text/plain;
    # Exercise keepalive after its separately allocated header buffer is freed.
    client_header_buffer_size 8k;
    gzip on;
    gzip_min_length 1;
    gzip_types text/plain;
    server {
        listen 127.0.0.1:$httpPort;
        listen 127.0.0.1:$secondHttpPort;
$tlsConfig
        location = / {
            return 200 "$body";
        }
        location = /expect {
            return 200 "$reloadedBody";
        }
        location = /worker {
            return 200 "`$pid";
        }
        location = /sendfile.txt {
            root "$($html.Replace('\','/'))";
            sendfile on;
            aio off;
        }
        location = /gzip.txt {
            root "$($html.Replace('\','/'))";
        }
        location = /backpressure.bin {
            root "$($html.Replace('\','/'))";
            access_log "$($dir.Replace('\','/'))/tls-bytes.log" rc_tls_bytes;
            gzip off;
            sendfile off;
            add_header X-RC-Worker "`$pid";
        }
        location = /read-notify {
            add_header X-RC-Worker "`$pid";
            return 200 "`$arg_value";
        }
$aioLocation
        location ~ ^/regex/(.*)$ {
            return 200 "pcre2-ok";
        }
    }
}
$streamConfig
$mailConfig
"@
    Set-Content -LiteralPath $config -Value $conf -Encoding ASCII
    Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-t')

    $startArgs = @('-e', 'stderr', '-p', $dir, '-c', $config)
    Write-Host "Starting nginx with: $($script:Binary) $($startArgs -join ' ')" -ForegroundColor Yellow
    $start = Start-Process -FilePath $script:Binary -WorkingDirectory $script:Root -ArgumentList $startArgs -RedirectStandardError $stderr -PassThru -WindowStyle Hidden
    $udpClient = $null
    $partialClients = @{}
    try {
        Write-Host "Waiting for HTTP on port $httpPort..." -ForegroundColor Yellow
        Wait-Http $httpPort $body
        Write-Host "HTTP OK" -ForegroundColor Green
        if ($TestMail) {
            Test-MailFirstCommands $imapPort 'imap'
            Test-MailFirstCommands $pop3Port 'pop3'
        }
        if ($Backend -eq 'iocp') {
            $udpClient = New-Object -TypeName System.Net.Sockets.UdpClient
            Invoke-Udp $udpPort $udpBody $udpClient
        }

        [void] (Wait-TestProcesses $dir 3)
        $oldWorkers = @(Wait-Workers $dir $start.Id 2)
        Test-IdleListeners $httpPort $secondHttpPort
        for ($attempt = 0; $attempt -lt 32 -and $partialClients.Count -lt 2; $attempt++) {
            $partial = Start-PartialHttpRequest $httpPort
            if ($partialClients.ContainsKey($partial.WorkerId)) {
                $partial.Client.Close()
            } else {
                $partialClients[$partial.WorkerId] = $partial.Client
            }
        }
        foreach ($worker in $oldWorkers) {
            Assert-True ($partialClients.ContainsKey([int] $worker.ProcessId)) `
                "Could not hold old worker $($worker.ProcessId) open"
        }

        $reloaded = (Get-Content -Raw -LiteralPath $config).Replace(
            "return 200 `"$body`";", "return 200 `"$reloadedBody`";")
        Set-Content -LiteralPath $config -Value $reloaded -Encoding ASCII
        Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-s', 'reopen')
        Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-s', 'reload')
        Wait-Http $httpPort $reloadedBody
        if ($Backend -eq 'iocp') {
            Invoke-Udp $udpPort $udpBody $udpClient
        }
        $liveIds = @(Get-TestProcesses $dir | ForEach-Object { $_.ProcessId })
        foreach ($worker in $oldWorkers) {
            Assert-True ($liveIds -contains $worker.ProcessId) `
                "Old worker $($worker.ProcessId) exited while a request was held open"
        }

        foreach ($client in $partialClients.Values) { $client.Close() }
        $partialClients.Clear()
        $workers = @(Wait-Workers $dir $start.Id 2)
        $killedId = $workers[0].ProcessId
        Stop-Process -Id $killedId -Force
        Wait-Http $httpPort $reloadedBody
        [void] (Wait-Workers $dir $start.Id 2 $killedId)

        if ($TestMail) {
            Test-MailFirstCommands $imapPort 'imap'
            Test-MailFirstCommands $pop3Port 'pop3'
        }

        Test-ExpectContinue $httpPort $reloadedBody
        Test-ReadNotifications $httpPort
        if ($script:Tls) {
            Write-Host "Testing HTTPS on port $httpsPort..." -ForegroundColor Yellow
            if (Test-Path $stderr) {
                Write-Host "Nginx stderr log before HTTPS test:" -ForegroundColor Magenta
                Get-Content -LiteralPath $stderr | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
            }
            Wait-Https $httpsPort $reloadedBody
            $beforeTlsReads = @(Wait-Workers $dir $start.Id 2)
            Test-ExpectContinue $httpsPort $reloadedBody -Tls
            Test-ReadNotifications $httpsPort -Tls
            Test-TlsHandshakeReset $httpsPort
            Test-TlsBufferedReads $httpsPort
            Wait-Https $httpsPort $reloadedBody
            $afterTlsReads = @(Wait-Workers $dir $start.Id 2)
            Assert-True ((($beforeTlsReads.ProcessId | Sort-Object) -join ',') -eq `
                         (($afterTlsReads.ProcessId | Sort-Object) -join ',')) `
                'TLS read cancellation unexpectedly replaced a worker'
            $beforeBackpressure = @(Wait-Workers $dir $start.Id 2)
            Test-TlsBackpressure $httpsPort $tlsBody {
                param([int] $BlockedWorker)
                Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-s', 'reload')
                Wait-Http $httpPort $reloadedBody
                $liveIds = @(Get-TestProcesses $dir | ForEach-Object { $_.ProcessId })
                Assert-True ($liveIds -contains $BlockedWorker) `
                    'Reload lost the worker with a blocked TLS response'
            }
            $afterBackpressure = @(Wait-Workers $dir $start.Id 2)
            foreach ($old in $beforeBackpressure) {
                Assert-True ($afterBackpressure.ProcessId -notcontains $old.ProcessId) `
                    'TLS backpressure left an old worker alive after reload'
            }
            $tlsCounts = @(Get-Content -LiteralPath (Join-Path $dir 'tls-bytes.log') |
                ForEach-Object {
                    Assert-True ($_ -match '^\d+ (\d+)$') 'Invalid TLS byte accounting log'
                    [long] $Matches[1]
                })
            Assert-True (@($tlsCounts | Where-Object { $_ -eq $tlsBody.Length }).Count -ge 3) `
                'TLS backpressure did not account for three complete plaintext bodies'
            Assert-True (@($tlsCounts | Where-Object { $_ -gt $tlsBody.Length }).Count -eq 0) `
                'TLS byte accounting included encrypted transport overhead'
            Write-Host 'PASS plaintext byte accounting after blocked TLS writes'
            Wait-Https $httpsPort $reloadedBody
            Write-Host "HTTPS OK" -ForegroundColor Green
        }

        $sendfile = Get-HttpBody $httpPort '/sendfile.txt'
        Assert-True ($sendfile -eq $fileBody) "$Backend sendfile response mismatch"
        if ($Backend -eq 'iocp') {
            $aio = Get-HttpBody $httpPort '/aio.txt'
            Assert-True ($aio -eq $fileBody) 'IOCP file AIO response mismatch'
        }
        Test-Gzip $httpPort $fileBody
        $regex = Get-HttpBody $httpPort '/regex/pcre2-ok'
        Assert-True ($regex -eq 'pcre2-ok') "$Backend regex response mismatch"
    } finally {
        foreach ($client in $partialClients.Values) { $client.Close() }
        if ($udpClient) { $udpClient.Close() }
        Write-Host "Stopping nginx for backend $Backend..." -ForegroundColor Yellow
        Stop-TestNginx $dir $config $start
    }

    $logText = if (Test-Path $stderr) { Get-Content -Raw -LiteralPath $stderr } else { '' }
    if ($logText) {
        Write-Host "Final stderr log for $Backend (last 30 lines):" -ForegroundColor Magenta
        ($logText -split "`n") | Select-Object -Last 30 | ForEach-Object { Write-Host "  $_" }
    }
    Assert-True ($logText -notmatch '\[(?:emerg|alert|crit)\]|could not start|router failed|shutdown left pending') "$Backend emitted a fatal lifecycle error"
    $network = if ($Backend -eq 'iocp') {
        'HTTP/UDP, file AIO/sendfile'
    } else {
        'HTTP, static file'
    }
    $tls = if ($script:Tls) { ', TLS' } else { '' }
    $mail = if ($TestMail) { ', IMAP/POP3' } else { '' }
    Write-Host "PASS $Backend ($network, PCRE2, gzip$tls$mail, reopen, reload, respawn, quit)"
}

if ($Library) {
    return
}

Assert-True (Test-Path $Binary) "nginx binary not found: $Binary"
$script:Binary = [IO.Path]::GetFullPath($Binary)
$script:Root = [IO.Path]::GetFullPath($Root)
$script:OpenSSL = ''
$script:Tls = $null
$base = Join-Path ([IO.Path]::GetTempPath()) ('nginx-win32-rc-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $base | Out-Null

try {
    if ($OpenSSLBinary) {
        Assert-True (Test-Path $OpenSSLBinary) `
            "OpenSSL binary not found: $OpenSSLBinary"
        $script:OpenSSL = [IO.Path]::GetFullPath($OpenSSLBinary)
        $cert = Join-Path $base 'rc-cert.pem'
        $key = Join-Path $base 'rc-key.pem'
        $opensslConfig = Join-Path $base 'openssl.cnf'
        Set-Content -LiteralPath $opensslConfig -Encoding ASCII -Value @'
[req]
distinguished_name = dn
[dn]
'@
        Invoke-OpenSSL @('req', '-config', $opensslConfig, '-x509',
            '-newkey', 'rsa:2048', '-nodes',
            '-keyout', $key, '-out', $cert, '-subj', '/CN=localhost',
            '-days', '1')
        $script:Tls = @{ Cert = $cert; Key = $key }
    }

    foreach ($backend in @('iocp', 'select', 'poll')) {
        Test-Backend $backend $base
    }

    $emptyName = 'nginx-rc-empty-' + [Guid]::NewGuid().ToString('N')
    $emptyDir = Split-Path $script:Binary -Parent
    $emptyConfigName = "$emptyName.conf"
    $emptyConfig = Join-Path $emptyDir $emptyConfigName
    $emptyPidName = "$emptyName.pid"
    $emptyPort = Get-FreeTcpPort
    Set-Content -LiteralPath $emptyConfig -Value @"
worker_processes 1;
error_log stderr notice;
pid $emptyPidName;
events { use select; worker_connections 64; }
http {
    access_log stderr;
    server { listen 127.0.0.1:$emptyPort; return 200 "empty-prefix"; }
}
"@ -Encoding ASCII
    try {
        Invoke-NginxEmptyPrefix $emptyConfigName
    } finally {
        Remove-Item -LiteralPath $emptyConfig -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $emptyDir $emptyPidName) -Force -ErrorAction SilentlyContinue
    }
    Write-Host 'PASS empty prefix configuration'
    Write-Host 'Windows RC matrix completed successfully.'
} finally {
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "Artifacts retained at $base"
    }
}
