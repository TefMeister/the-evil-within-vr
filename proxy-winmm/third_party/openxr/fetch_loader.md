# Fetching the OpenXR loader (not committed)

The headers in `include/openxr/` are vendored (Khronos OpenXR 1.0.10, Apache-2.0 OR MIT), copied from the
Alan Wake proxy. The loader binary is gitignored (redistributable, and repo size limits).

The Evil Within is 64-bit, so it needs the **x64** loader from the `OpenXR.Loader` NuGet package, version 1.0.10.2:

```
curl -sSL -o oxr.nupkg \
  https://api.nuget.org/v3-flatcontainer/openxr.loader/1.0.10.2/openxr.loader.1.0.10.2.nupkg
unzip oxr.nupkg -d oxr
cp oxr/native/x64/release/bin/openxr_loader.dll third_party/openxr/bin/
```

Fetched 2026-10-06: SHA-256 `7d0a7cbb3fd2fe6bebd8fb6717e2434223483a9d79e0b93b47395f6dd65272ad` (PE32+, x86-64).

No `.lib` is needed: the proxy never links the loader. It loads `openxr_loader.dll` from beside `EvilWithin.exe`
at run time, and only when `OPENXR = 1` is set in `tewvr.ini`.
