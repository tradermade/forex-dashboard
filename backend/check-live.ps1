# Optional smoke check: contacts TraderMade using credentials already loaded by the gateway.
param([string]$Symbol = 'EURUSD', [string]$Timeframe = '1h')
$ErrorActionPreference = 'Stop'
$base = 'http://127.0.0.1:3001'
$health = Invoke-RestMethod "$base/api/health"
if ($health.backend -ne 'cpp') { throw 'Expected the C++ backend on port 3001.' }
$history = Invoke-RestMethod "$base/api/history?symbol=$([Uri]::EscapeDataString($Symbol))&timeframe=$([Uri]::EscapeDataString($Timeframe))"
if ($history.candles.Count -lt 1) { throw 'No historical candles returned.' }
Write-Output "C++ history: $Symbol, $($history.candles.Count) candles, cache: $($history.cache.layer)"
$socket = [Net.WebSockets.ClientWebSocket]::new()
$timeout = [Threading.CancellationTokenSource]::new(20000)
try {
    [void]$socket.ConnectAsync([Uri]'ws://127.0.0.1:3001/ws', $timeout.Token).GetAwaiter().GetResult()
    $bytes = [Text.Encoding]::UTF8.GetBytes((@{type='subscribe';symbol=$Symbol} | ConvertTo-Json -Compress))
    [void]$socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, $timeout.Token).GetAwaiter().GetResult()
    $buffer = New-Object byte[] 8192
    while ($true) {
        $message = [Text.StringBuilder]::new()
        do {
            $received = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $timeout.Token).GetAwaiter().GetResult()
            if ($received.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { throw 'Stream closed before a quote arrived.' }
            [void]$message.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $received.Count))
        } while (!$received.EndOfMessage)
        $data = $message.ToString() | ConvertFrom-Json
        if ($data.type -eq 'quote') {
            Write-Output "C++ stream: $($data.quote.symbol), bid $($data.quote.bid), ask $($data.quote.ask), cached: $($data.quote.cached)"
            break
        }
        if ($data.state -eq 'error') { throw $data.message }
    }
} finally { $socket.Dispose(); $timeout.Dispose() }
