# 分块备份板上固件。串口在长读时偶发掉线,所以按块读并自动重试,
# 已完成的块会跳过,中断后重跑不会从头开始。
#
# 用法:
#   . D:\ESP32P4C5\tools\idf.ps1
#   .\tools\backup_flash.ps1 -Port COM6 -Baud 460800 -OutDir D:\ESP32P4C5\ESP32P4\backup\chunks

param(
    [string]$Port = 'COM6',
    [int]$Baud = 460800,
    [string]$OutDir = 'D:\ESP32P4C5\ESP32P4\backup\chunks',
    [int]$ChunkSize = 0x100000,
    [int]$ChunkCount = 16,
    [int]$Retries = 4
)

$python = 'D:\ESP32P4C5\.tools\idf_py_venv\Scripts\python.exe'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

for ($i = 0; $i -lt $ChunkCount; $i++) {
    $offset = $i * $ChunkSize
    $file = Join-Path $OutDir ('chunk_{0:D2}.bin' -f $i)

    if ((Test-Path $file) -and ((Get-Item $file).Length -eq $ChunkSize)) {
        Write-Host ("chunk {0} already complete" -f $i)
        continue
    }

    $ok = $false
    for ($attempt = 1; $attempt -le $Retries -and -not $ok; $attempt++) {
        $out = & $python -m esptool --chip esp32p4 -p $Port -b $Baud read-flash `
            ('0x{0:x}' -f $offset) ('0x{0:x}' -f $ChunkSize) $file 2>&1
        if ((Test-Path $file) -and ((Get-Item $file).Length -eq $ChunkSize)) {
            $ok = $true
            Write-Host ("chunk {0} ok (attempt {1})" -f $i, $attempt) -ForegroundColor Green
        }
        else {
            $tail = ($out | Select-Object -Last 2) -join ' | '
            Write-Host ("chunk {0} failed attempt {1}: {2}" -f $i, $attempt, $tail) -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }

    if (-not $ok) {
        Write-Host ("chunk {0} gave up" -f $i) -ForegroundColor Red
        exit 1
    }
}

$target = 'D:\ESP32P4C5\ESP32P4\backup\vendor_demo_16MB.bin'
$stream = [System.IO.File]::Create($target)
try {
    for ($i = 0; $i -lt $ChunkCount; $i++) {
        $file = Join-Path $OutDir ('chunk_{0:D2}.bin' -f $i)
        $bytes = [System.IO.File]::ReadAllBytes($file)
        $stream.Write($bytes, 0, $bytes.Length)
    }
}
finally {
    $stream.Close()
}

$hash = (Get-FileHash $target -Algorithm SHA256).Hash
Write-Host ("merged -> {0}  ({1} bytes)" -f $target, (Get-Item $target).Length) -ForegroundColor Green
Write-Host ("sha256 = {0}" -f $hash)
