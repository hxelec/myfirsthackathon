$ErrorActionPreference = 'Stop'
$sketch = Split-Path -Parent $PSScriptRoot
$output = Join-Path $sketch 'src/audio/VoiceData.h'
$names = @('calibrate', 'startrangeofmotion', 'startconcentric',
           'endrangeofmotion', 'starteccentric', 'letsgo', 'starttracking', 'settings', 'notes', 'timer', 'help')
$repNumbers = 1..20
$availableReps = @{}
$data = [Text.StringBuilder]::new()
[void]$data.AppendLine('// Generated from sfx/*.mp3 by extras/BuildVoice.ps1. 16 kHz mono PCM.')
[void]$data.AppendLine('#pragma once')
[void]$data.AppendLine('#include <cstddef>')
[void]$data.AppendLine('namespace voice {')
[void]$data.AppendLine('struct Clip { const char *pcm; std::size_t bytes; };')
$temp = [IO.Path]::GetTempFileName()
try {
    foreach ($name in ($names + $repNumbers)) {
        $inputFile = Join-Path $sketch "sfx/$name.mp3"
        $symbol = $name
        if ($name -is [int]) {
            if (![IO.File]::Exists($inputFile)) { continue }
            $availableReps[$name] = $true
            $symbol = "rep$name"
        }
        & ffmpeg -nostdin -v error -y -i $inputFile -ac 1 -ar 16000 -c:a pcm_s16le -f s16le $temp
        if ($LASTEXITCODE -ne 0) { throw "Could not convert $inputFile" }
        $bytes = [IO.File]::ReadAllBytes($temp)
        if ($bytes.Length -eq 0) { throw "No audio in $inputFile" }
        [void]$data.AppendLine("static constexpr char $symbol[] =")
        for ($i = 0; $i -lt $bytes.Length; $i += 32) {
            $end = [Math]::Min($i + 32, $bytes.Length)
            $line = [Text.StringBuilder]::new()
            for ($j = $i; $j -lt $end; ++$j) {
                [void]$line.Append(('\x{0:x2}' -f $bytes[$j]))
            }
            [void]$data.AppendLine('    "' + $line.ToString() + '"')
        }
        [void]$data.AppendLine('    ;')
    }
    [void]$data.AppendLine('static constexpr Clip reps[] = {')
    foreach ($rep in $repNumbers) {
        if ($availableReps.ContainsKey($rep)) {
            [void]$data.AppendLine("    {rep$rep, sizeof(rep$rep) - 1},")
        } else {
            [void]$data.AppendLine("    {nullptr, 0},  // $rep.mp3")
        }
    }
    [void]$data.AppendLine('};')
    [void]$data.AppendLine('}')
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $output))
    [IO.File]::WriteAllText($output, $data.ToString())
} finally {
    [IO.File]::Delete($temp)
}
