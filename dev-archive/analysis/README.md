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
fresh capture and is kept out of the repository by `.gitignore` (`*.log`).

**Where the local copy is (updated 2026-08-31).** It lives on the dev PC only, at
`D:\TheEvilWithinVR\captures\` — a folder outside every repository, left in place
deliberately during the post-consolidation cleanup. The *screenshots* from that folder that
back the Phase 4 keystone proof were rescued into the private staging repo at
`staging/the-evil-within-vr/captures-evidence/`; the ~60 MB of `.log` dumps were not, per the
policy above. If that dev-PC folder is deleted, the raw logs are gone — acceptable, but it
should be a decision rather than an accident.

These scripts read only our own instrumentation output — decoded constant-buffer
floats, which are interoperability data about how the engine feeds the GPU. No
game files are read or included.

**`analyze_seqdump.py`** was added on 2026-08-31. It had existed only in the
dev-PC capture folder, never in any repository, and would have been lost with it.
