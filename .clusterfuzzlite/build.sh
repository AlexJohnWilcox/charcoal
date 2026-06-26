#!/bin/bash -eu
# ClusterFuzzLite entry point. Builds every Charcoal harness into $OUT.
# CFLite sets $CXX, $CXXFLAGS, $LIB_FUZZING_ENGINE, $SRC, $OUT and runs with
# the repo as the working directory. We reference sources via $SRC.

# A single wrapping top-level folder is unwrapped automatically, so the repo
# root may be $SRC or $SRC/charcoal.
ROOT="$SRC"
[ -d "$SRC/charcoal/src" ] && ROOT="$SRC/charcoal"

INC="-I$ROOT/include -I$ROOT/src"

build_harness() {
  local name="$1"
  [ -f "$ROOT/fuzz/$name.cc" ] || return 0
  # Compile every core TU + the harness directly. This is the robust path for
  # CFLite (no dependence on CMake artifact locations).
  $CXX $CXXFLAGS $INC \
    "$ROOT"/src/*.cc "$ROOT/fuzz/$name.cc" \
    $LIB_FUZZING_ENGINE -o "$OUT/$name"
}

build_harness module_fuzzer

# Package seed corpus if present (CFLite picks up $OUT/<harness>_seed_corpus.zip).
for h in module_fuzzer; do
  if [ -d "$ROOT/fuzz/corpus/$h" ]; then
    ( cd "$ROOT/fuzz/corpus/$h" && zip -qr "$OUT/${h}_seed_corpus.zip" . ) || true
  fi
done

# Ship the fuzzing dictionary if present.
[ -f "$ROOT/fuzz/dictionary.txt" ] && cp "$ROOT/fuzz/dictionary.txt" "$OUT/module_fuzzer.dict" || true
