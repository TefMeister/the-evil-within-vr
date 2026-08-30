# Analysis scripts

Offline Python used to find the camera view matrix (Task 4) from a
constant-buffer capture. They parse `cbdump.log` — the file written by the
temporary `TEWVR_DUMP=1` instrumentation, which decodes camera-sized constant
buffers as 4×4 float matrices — from `%LOCALAPPDATA%\TEWVR\cbdump.log`.

- **`analyze_cb.py`** — first pass. For every buffer, scores its first two 4×4
  matrices for a view signature (orthonormal 3×3 rotation that varies over time)
  and a projection signature (sparse, static).
- **`slide.py`** — the decisive pass. Slides a 16-float window across every
  buffer at all offsets and reports which windows are an orthonormal rotation
  that changes across records (the view matrix) and which match a static
  perspective-projection pattern. This is what isolated the view matrix to byte
  offset 48 of a 384-byte double-buffered dynamic constant buffer.
- **`examine.py`** — prints selected buffers' matrices across the capture, for
  eyeballing how a candidate changes as the camera moves.

The finding they produced is written up in
[../notes/06-camera-matrix-discovery.md](../notes/06-camera-matrix-discovery.md).

The raw capture itself (~15 MB) is not stored here — it is regenerable from a
fresh capture and is kept out of the repository by `.gitignore`. A local copy
lives under `D:\TheEvilWithinVR\captures\`.

These scripts read only our own instrumentation output — decoded constant-buffer
floats, which are interoperability data about how the engine feeds the GPU. No
game files are read or included.
