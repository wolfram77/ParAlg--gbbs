#!/usr/bin/env bash
src="ParAlg--gbbs"
out="$HOME/Logs/$src.log"
ulimit -s unlimited
printf "" > "$out"

# Download GBBS
if [[ "$DOWNLOAD" != "0" ]]; then
  rm -rf $src
  git clone https://github.com/wolfram77/$src
fi
cd $src

# Build GBBS
bazel build //...

# Convert graph to GBBS format, run GBBS, and clean up
runGbbs() {
  stdbuf --output=L printf "Converting $1 to $1.edges ...\n"                               | tee -a "$out"
  lines="$(node process.js header-lines "$1")"
  tail -n +$((lines+1)) "$1" > "$1.edges"
  stdbuf --output=L bazel run //utils:snap_converter -- -s -i "$1.edges" -o "$1.gbbs" 2>&1 | tee -a "$out"
  stdbuf --output=L bazel run //benchmarks/PageRank:PageRank_main -- "$1.gbbs"        2>&1 | tee -a "$out"
  stdbuf --output=L printf "\n\n"                                                          | tee -a "$out"
  rm -rf "$1.edges"
  rm -rf "$1.gbbs"
}

# Run GBBS on all graphs
runAll() {
  # runGbbs "$HOME/Data/web-Stanford.mtx"
  runGbbs "$HOME/Data/indochina-2004.mtx"
  runGbbs "$HOME/Data/uk-2002.mtx"
  runGbbs "$HOME/Data/arabic-2005.mtx"
  runGbbs "$HOME/Data/uk-2005.mtx"
  runGbbs "$HOME/Data/webbase-2001.mtx"
  runGbbs "$HOME/Data/it-2004.mtx"
  runGbbs "$HOME/Data/sk-2005.mtx"
  runGbbs "$HOME/Data/com-LiveJournal.mtx"
  runGbbs "$HOME/Data/com-Orkut.mtx"
  runGbbs "$HOME/Data/asia_osm.mtx"
  runGbbs "$HOME/Data/europe_osm.mtx"
  runGbbs "$HOME/Data/kmer_A2a.mtx"
  runGbbs "$HOME/Data/kmer_V1r.mtx"
}

# Run GBBS 5 times
for i in {1..5}; do
  runAll
done

# Signal completion
curl -X POST "https://maker.ifttt.com/trigger/puzzlef/with/key/${IFTTT_KEY}?value1=$src$1"
