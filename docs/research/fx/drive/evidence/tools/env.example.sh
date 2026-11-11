# Environment the evidence tools expect. RE_STATE is the shared reverse-engineering state directory of the study
# (it holds bin/auhost, the thin arm64 slices and the symbol index); W is a scratch work directory.
export RE_STATE=${RE_STATE:?set RE_STATE to the shared state directory}
export RE=${RE:?set RE to <repo>/tools/re}
export W=${W:-$RE_STATE/work/drive}
export C=/Library/Audio/Plug-Ins/Components
export GR6="$C/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
export AT5="$C/AmpliTube 5.component/Contents/MacOS/AmpliTube 5"
mkdir -p "$W/tools" "$W/wav" "$W/meas"
# models.py looks for the tubamp state with a drive-only chain at $W/ours_drive_only.aupreset (copy it from this folder).
