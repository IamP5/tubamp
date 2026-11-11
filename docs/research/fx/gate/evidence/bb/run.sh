#!/bin/zsh
# run.sh <tag> <auhost render args...>   e.g. run.sh gw aufx 72RG AAMa --set 1=-40
# Renders every test signal through the plugin and prints the analyses. Tag "ours:<thr>" runs the local harness.
export RE_STATE=/private/tmp/claude-501/-Users-tuba-Dev-projects-tubamp--claude-worktrees-tubamp-effects-reverse-engineer-90c7f7/cab46780-7fe6-4ba5-87cd-2d4971b7e79d/scratchpad/re
RE=/Users/tuba/Dev/projects/tubamp/.claude/worktrees/tubamp-effects-reverse-engineer-90c7f7/tools/re
cd ${0:A:h}
tag=$1; shift
for s in decay rise burst burstlf hover chug sustain; do
  if [[ $tag == ext:* ]]; then :
  elif [[ $tag == ours:* ]]; then ./ours sig/$s.wav out/${tag/:/_}_$s.wav ${tag#ours:} 512
  else $RE_STATE/bin/auhost render "$1" "$2" "$3" sig/$s.wav out/${tag}_$s.wav "${@:4}" > out/${tag}_$s.log 2>&1 || { echo "render failed $s: $(tail -1 out/${tag}_$s.log | cut -c1-150)"; exit 1; }
  fi
done
t=${tag/:/_}; t=${t#ext_}
echo "# $tag :: $@"
echo "decay_static(in_dB gain_dB): $(python3 an.py static sig/decay.wav out/${t}_decay.wav $RE 2>/dev/null | tr '\t\n' ': ')"
echo "rise_static: $(python3 an.py static sig/rise.wav out/${t}_rise.wav $RE 2>/dev/null | tr '\t\n' ': ')"
echo "decay_thr: $(python3 an.py thr sig/decay.wav out/${t}_decay.wav $RE 2>/dev/null | sed -E 's/first t=[0-9.]+s //; s/ ; last.*//' | tr '\n' '|')"
echo "rise_thr: $(python3 an.py thr sig/rise.wav out/${t}_rise.wav $RE 2>/dev/null | sed -E 's/first.*; last t=[0-9.]+s //' | tr '\n' '|')"
echo "burst1k:"; python3 an.py burst sig/burst.wav out/${t}_burst.wav $RE 2>/dev/null | sed -n '1p;3p'
echo "burst82:"; WIN=1165 python3 an.py burst sig/burstlf.wav out/${t}_burstlf.wav $RE 2>/dev/null | sed -n '1p;3p'
echo "hover: $(python3 an.py flips sig/hover.wav out/${t}_hover.wav $RE 2>/dev/null)"
echo "chug: $(python3 an.py gaps sig/chug.wav out/${t}_chug.wav $RE 2>/dev/null)"
echo "sustain: $(python3 an.py sus sig/sustain.wav out/${t}_sustain.wav $RE 2>/dev/null | tr '\t\n' ': ')"
