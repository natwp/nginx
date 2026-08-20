<#
.SYNOPSIS
    Runs the native Windows release-candidate smoke and lifecycle matrix.

.DESCRIPTION
    Uses only loopback sockets and a unique temporary prefix. It exercises
    IOCP, select, and poll with two workers, then checks reload, reopen, worker
    replacement, file paths, PCRE2, gzip, optional TLS, and stderr logging.
#>

[CmdletBinding()]
param(
    [string] $Binary = (Join-Path $PSScriptRoot '..\objs\nginx.exe'),
    [string] $Root = (Join-Path $PSScriptRoot '..'),
    [string] $OpenSSLBinary = '',
    [switch] $SkipStream,
    [switch] $KeepArtifacts
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

function Wait-Http([int] $Port, [string] $Expected, [int] $TimeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        try {
            $client = New-Object System.Net.WebClient
            $client.Proxy = $null
            $body = $client.DownloadString("http://127.0.0.1:$Port/")
            if ($body -eq $Expected) {
                return
            }
        } catch {
            Start-Sleep -Milliseconds 200
        }
    } while ((Get-Date) -lt $deadline)
    throw "HTTP backend on port $Port did not return '$Expected'"
}

function Test-ConnectionChurn([int] $Port, [string] $Expected,
    [int] $Concurrency = 32, [int] $Rounds = 100) {
    if (-not ('NginxWin32Rc.ConnectionChurn' -as [type])) {
        Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;

namespace NginxWin32Rc
{
    public static class ConnectionChurn
    {
        private static readonly byte[] Request = Encoding.ASCII.GetBytes(
            "GET /churn HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
            "Connection: close\r\n\r\n");

        public static void Run(int port, string expected, int concurrency,
            int rounds)
        {
            for (int round = 0; round < rounds; round++) {
                Task[] tasks = new Task[concurrency];

                for (int i = 0; i < concurrency; i++) {
                    tasks[i] = SendRequest(port, expected);
                }

                if (!Task.WaitAll(tasks, 20000)) {
                    throw new TimeoutException(
                        "HTTP connection-churn batch timed out");
                }
            }
        }

        private static async Task SendRequest(int port, string expected)
        {
            using (TcpClient client = new TcpClient()) {
                client.NoDelay = true;
                await client.ConnectAsync("127.0.0.1", port)
                    .ConfigureAwait(false);

                using (NetworkStream stream = client.GetStream())
                using (MemoryStream response = new MemoryStream()) {
                    await stream.WriteAsync(Request, 0, Request.Length)
                        .ConfigureAwait(false);

                    byte[] buffer = new byte[1024];
                    int size;

                    while ((size = await stream.ReadAsync(buffer, 0,
                                                          buffer.Length)
                                              .ConfigureAwait(false)) != 0)
                    {
                        response.Write(buffer, 0, size);

                        if (response.Length > 65536) {
                            throw new InvalidDataException(
                                "HTTP churn response was too large");
                        }
                    }

                    string text = Encoding.ASCII.GetString(
                        response.ToArray());

                    if (!text.StartsWith("HTTP/1.1 200 ",
                                         StringComparison.Ordinal)
                        || text.IndexOf("\r\n\r\n" + expected,
                                        StringComparison.Ordinal) < 0)
                    {
                        throw new InvalidDataException(
                            "HTTP churn response mismatch");
                    }
                }
            }
        }
    }
}
'@
    }

    [NginxWin32Rc.ConnectionChurn]::Run($Port, $Expected, $Concurrency,
        $Rounds)
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

function Wait-Https([int] $Port, [string] $Expected, [int] $TimeoutSec = 20) {
    $oldCallback = [Net.ServicePointManager]::ServerCertificateValidationCallback
    $oldProtocol = [Net.ServicePointManager]::SecurityProtocol
    try {
        [Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        do {
            try {
                $client = New-Object System.Net.WebClient
                $client.Proxy = $null
                $body = $client.DownloadString("https://127.0.0.1:$Port/")
                if ($body -eq $Expected) {
                    return
                }
            } catch {
                Start-Sleep -Milliseconds 200
            }
        } while ((Get-Date) -lt $deadline)
        throw "HTTPS backend on port $Port did not return '$Expected'"
    } finally {
        [Net.ServicePointManager]::ServerCertificateValidationCallback = $oldCallback
        [Net.ServicePointManager]::SecurityProtocol = $oldProtocol
    }
}

function Test-Gzip([int] $Port, [string] $Expected) {
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
    } finally {
        if ($reader) { $reader.Dispose() }
        elseif ($gzip) { $gzip.Dispose() }
        if ($response) { $response.Dispose() }
    }
}

function Invoke-Udp([int] $Port, [string] $Expected) {
    $client = New-Object -TypeName System.Net.Sockets.UdpClient
    $client.Client.ReceiveTimeout = 5000
    $remote = New-Object -TypeName System.Net.IPEndPoint -ArgumentList ([IPAddress]::Loopback, $Port)
    $payload = [Text.Encoding]::ASCII.GetBytes('win32-rc')
    [void] $client.Send($payload, $payload.Length, $remote)
    $source = New-Object -TypeName System.Net.IPEndPoint -ArgumentList ([IPAddress]::Any, 0)
    $reply = $client.Receive([ref] $source)
    $client.Close()
    $text = [Text.Encoding]::ASCII.GetString($reply)
    Assert-True ($text -eq $Expected) "UDP returned '$text', expected '$Expected'"
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
    $dir = Join-Path $BaseDir $Backend
    $logs = Join-Path $dir 'logs'
    $html = Join-Path $dir 'html'
    $temp = Join-Path $dir 'temp'
    New-Item -ItemType Directory -Force -Path $logs, $html, $temp |
        Out-Null

    $httpPort = Get-FreeTcpPort
    $httpsPort = Get-FreeTcpPort
    $udpPort = Get-FreeUdpPort
    $config = Join-Path $dir 'nginx.conf'
    $pidFile = Join-Path $logs 'nginx.pid'
    $stderr = Join-Path $logs 'stderr.log'
    $body = "backend=$Backend"
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
    $streamConfig = if ($Backend -eq 'iocp' -and $script:Stream) {
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
    $tlsConfig = if ($script:Tls) {
@"
        listen 127.0.0.1:$httpsPort ssl;
        ssl_certificate "$($script:Tls.Cert.Replace('\','/'))";
        ssl_certificate_key "$($script:Tls.Key.Replace('\','/'))";
"@
    } else { '' }
    foreach ($file in @('aio.txt', 'gzip.txt', 'sendfile.txt')) {
        Set-Content -LiteralPath (Join-Path $html $file) -Value $fileBody `
            -NoNewline -Encoding ASCII
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
    default_type text/plain;
    gzip on;
    gzip_min_length 1;
    gzip_types text/plain;
    server {
        listen 127.0.0.1:$httpPort;
$tlsConfig
        location = / {
            return 200 "$body";
        }
        location = /churn {
            access_log off;
            return 200 "$body";
        }
        location = /sendfile.txt {
            root "$($html.Replace('\','/'))";
            sendfile on;
            aio off;
        }
        location = /gzip.txt {
            root "$($html.Replace('\','/'))";
        }
$aioLocation
        location ~ ^/regex/(.*)$ {
            return 200 "pcre2-ok";
        }
    }
}
$streamConfig
"@
    Set-Content -LiteralPath $config -Value $conf -Encoding ASCII
    Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-t')

    $startArgs = @('-e', 'stderr', '-p', $dir, '-c', $config)
    $start = Start-Process -FilePath $script:Binary -WorkingDirectory $script:Root -ArgumentList $startArgs -RedirectStandardError $stderr -PassThru -WindowStyle Hidden
    try {
        Wait-Http $httpPort $body
        if ($Backend -eq 'iocp') {
            Test-ConnectionChurn $httpPort $body
        }
        if ($Backend -eq 'iocp' -and $script:Stream) {
            Invoke-Udp $udpPort $udpBody
        }

        [void] (Wait-TestProcesses $dir 3)
        [void] (Wait-Workers $dir $start.Id 2)
        Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-s', 'reopen')
        Invoke-Nginx @('-e', 'stderr', '-p', $dir, '-c', $config, '-s', 'reload')
        Wait-Http $httpPort $body

        $workers = @(Wait-Workers $dir $start.Id 2)
        $killedId = $workers[0].ProcessId
        Stop-Process -Id $killedId -Force
        Wait-Http $httpPort $body
        [void] (Wait-Workers $dir $start.Id 2 $killedId)

        if ($script:Tls) {
            Wait-Https $httpsPort $body
        }

        $sendfile = (New-Object System.Net.WebClient).DownloadString("http://127.0.0.1:$httpPort/sendfile.txt")
        Assert-True ($sendfile -eq $fileBody) "$Backend sendfile response mismatch"
        if ($Backend -eq 'iocp') {
            $aio = (New-Object System.Net.WebClient).DownloadString("http://127.0.0.1:$httpPort/aio.txt")
            Assert-True ($aio -eq $fileBody) 'IOCP file AIO response mismatch'
        }
        Test-Gzip $httpPort $fileBody
        $regex = (New-Object System.Net.WebClient).DownloadString("http://127.0.0.1:$httpPort/regex/pcre2-ok")
        Assert-True ($regex -eq 'pcre2-ok') "$Backend regex response mismatch"
    } finally {
        Stop-TestNginx $dir $config $start
    }

    $logText = if (Test-Path $stderr) { Get-Content -Raw -LiteralPath $stderr } else { '' }
    Assert-True ($logText -notmatch '\[(?:emerg|alert|crit)\]|could not start|router failed|shutdown left pending') "$Backend emitted a fatal lifecycle error"
    $network = if ($Backend -eq 'iocp' -and $script:Stream) {
        'HTTP/UDP, file AIO/sendfile'
    } elseif ($Backend -eq 'iocp') {
        'HTTP, file AIO/sendfile'
    } else {
        'HTTP, static file'
    }
    $tls = if ($script:Tls) { ', TLS' } else { '' }
    Write-Host "PASS $Backend ($network, PCRE2, gzip$tls, reopen, reload, respawn, quit)"
}

Assert-True (Test-Path $Binary) "nginx binary not found: $Binary"
$script:Binary = [IO.Path]::GetFullPath($Binary)
$script:Root = [IO.Path]::GetFullPath($Root)
$script:OpenSSL = ''
$script:Tls = $null
$script:Stream = -not $SkipStream
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
    $emptyTemp = Join-Path $base 'empty-temp'
    New-Item -ItemType Directory -Force -Path $emptyTemp | Out-Null
    $emptyTempPath = $emptyTemp.Replace('\','/')
    Set-Content -LiteralPath $emptyConfig -Value @"
worker_processes 1;
error_log stderr notice;
pid $emptyPidName;
events { use select; worker_connections 64; }
http {
    access_log stderr;
    client_body_temp_path "$emptyTempPath/client_body_temp";
    proxy_temp_path "$emptyTempPath/proxy_temp";
    fastcgi_temp_path "$emptyTempPath/fastcgi_temp";
    uwsgi_temp_path "$emptyTempPath/uwsgi_temp";
    scgi_temp_path "$emptyTempPath/scgi_temp";
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
