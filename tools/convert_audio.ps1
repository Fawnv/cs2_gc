# Converts an audio file (m4a, mp3, wav, wma, ...) to a 44.1 kHz mp3 or a 16 bit wav with Windows' own
# codecs (WinRT MediaTranscoder), for tools\make_music_mod.py and make_sound_mod.py when ffmpeg isn't installed.
#   powershell -ExecutionPolicy Bypass -File tools\convert_audio.ps1 musics\track.m4a musics\track.cs2gc.mp3
#   powershell -ExecutionPolicy Bypass -File tools\convert_audio.ps1 shot.mp3 shot.wav -Format wav
# -Channels is for mp3 only; the wav comes out 48 kHz stereo (see below).
param([Parameter(Mandatory)][string]$In, [Parameter(Mandatory)][string]$Out,
      [ValidateSet('mp3', 'wav')][string]$Format = 'mp3', [ValidateSet(1, 2)][int]$Channels = 2)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime]

# WinRT IAsyncOperation<T> / IAsyncAction -> wait for the result
$asTaskGeneric = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' }
function Await($op, [Type]$type) {
    $task = $asTaskGeneric.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait() | Out-Null
    $task.Result
}
function AwaitAction($action) {
    $task = [System.WindowsRuntimeSystemExtensions]::AsTask($action)
    $task.Wait() | Out-Null
}
function AwaitProgress($op) {
    $method = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1' }
    $task = $method.MakeGenericMethod([double]).Invoke($null, @($op))
    $task.Wait() | Out-Null
}

$inPath = (Resolve-Path $In).Path
$outFull = [System.IO.Path]::GetFullPath($Out)
$outDir = Split-Path $outFull
$outName = Split-Path $outFull -Leaf

$source = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($inPath)) ([Windows.Storage.StorageFile])
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($outDir)) ([Windows.Storage.StorageFolder])
$target = Await ($folder.CreateFileAsync($outName, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])

$quality = [Windows.Media.MediaProperties.AudioEncodingQuality]::High
if ($Format -eq 'wav') {
    # 16 bit pcm in the profile's own rate and channels (48 kHz stereo): any other rate or channel count
    # makes the transcoder fail with "Unknown", the caller resamples and mixes the channels itself
    $profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateWav($quality)
} else {
    $profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateMp3($quality)
    $profile.Audio.SampleRate = 44100
    $profile.Audio.ChannelCount = $Channels
}

$transcoder = New-Object Windows.Media.Transcoding.MediaTranscoder
$prepared = Await ($transcoder.PrepareFileTranscodeAsync($source, $target, $profile)) ([Windows.Media.Transcoding.PrepareTranscodeResult])
if (-not $prepared.CanTranscode) {
    throw "can't convert ${In}: $($prepared.FailureReason)"
}
AwaitProgress ($prepared.TranscodeAsync())
Write-Output "wrote $outFull"
