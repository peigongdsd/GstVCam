# GstVCam (VCamSample)

Windows 11 Media Foundation virtual camera backed by a GStreamer pipeline (`NV12 -> appsink`).

## What It Is

- `VCamSampleSource` (DLL): the actual Media Foundation virtual camera source loaded by Windows Frame Server.
- `VCamSample` (EXE): optional helper/launcher (thin).

At runtime, the source reads pipeline settings from registry and pushes frames from GStreamer `appsink` into the virtual camera stream.

## Requirements

- Windows 11 (virtual camera API support)
- Visual Studio C++ build tools
- GStreamer MSVC x64 runtime installed, typically:
  - `C:\Program Files\gstreamer\1.0\msvc_x86_64`
- GStreamer `bin` in **System PATH**:
  - `C:\Program Files\gstreamer\1.0\msvc_x86_64\bin`

## Build

- Build `x64` (`Debug` or `Release`) from `VCamSample.sln`.

## Register / Unregister

Run as Administrator from the folder containing `VCamSampleSource.dll`:

```bat
regsvr32 VCamSampleSource.dll
```

Remove:

```bat
regsvr32 /u VCamSampleSource.dll
```

## Pipeline Configuration

Registry key:

- `HKLM\SOFTWARE\VCamSample\GStreamer`

Values used:

- `Pipeline` (REG_SZ)
- `Width` (DWORD)
- `Height` (DWORD)
- `FpsNumerator` (DWORD)
- `FpsDenominator` (DWORD)

Example pipeline:

```text
videotestsrc is-live=true ! videoconvert ! video/x-raw,format=NV12,width=1280,height=960,framerate=30/1 ! appsink name=vcamsink
```

## Logging

- File log: `C:\vcamsample_trace.txt`
- Includes source startup, pipeline state, bus errors/warnings, and frame fallback messages.

## Troubleshooting

- `The specified module could not be found` on `regsvr32` usually means missing dependency DLLs (often GStreamer runtime not resolvable).
- Ensure architecture matches (`x64` DLL with x64 GStreamer).
- Ensure output DLL path is accessible by Frame Server service accounts.
