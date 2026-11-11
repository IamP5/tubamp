# Removed from this folder

`gr6_mxr.py` and `gr6_mxr_static.py` were removed on 2026-11-11.

They were an analysis-only transcription of Guitar Rig 6's Phaser Nine process loop,
including the vendor's own polynomial constants. The study used them to compute what
that code predicts (the sweep figures tagged `[inferred: evidence/tools/gr6_mxr_static.py]`
in `report.md` and `decompiled_notes.md`). A transcription of vendor constants does not
belong in this repository, so only the numbers it produced are kept. They can be
reproduced by reading the functions named in `decompiled_notes.md` section 3.

Nothing in `voicings.md`, `prototype/` or `evidence/fit/` used these scripts.
