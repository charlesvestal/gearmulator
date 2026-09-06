# Profile-guided optimisation for the ESP interpreter

`je8086.profdata` is a merged clang profile for the interpreted ESP engine.
Building with it is worth **+45% serial / +50% pipelined**, bit-exact — more
than every hand optimisation in this series combined, from a build flag.

That is not a surprise in hindsight: the interpreter is a large switch in a hot
loop over a program the branch predictor can learn, which is precisely the shape
PGO exists to fix. It should have been tried first, not last.

## Rebuilding the profile

Regenerate whenever `esp.hpp`'s step loop changes materially; a stale profile
degrades quietly rather than failing.

    cmake -S . -B build-pgo-gen -DCMAKE_BUILD_TYPE=Release -G Ninja \
      -DCMAKE_CXX_FLAGS="-fprofile-generate=/tmp/pgodata" \
      -DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate=/tmp/pgodata"
    cmake --build build-pgo-gen --target bench_je -j
    rm -rf /tmp/pgodata
    JE_NO_AUTO_PIPELINE=1 JE_ESP_INTERP=1 ./build-pgo-gen/bench_je dist/jp8000/roms
    xcrun llvm-profdata merge -output=pgo/je8086.profdata /tmp/pgodata/*.profraw

## Using it

    -DCMAKE_CXX_FLAGS="-fprofile-use=$PWD/pgo/je8086.profdata \
       -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled"

## It generalises

Trained on `bench_je` alone, then measured on scripts it never saw, with
byte-identical audio in every case:

| held-out script | baseline | PGO   | speedup |
|-----------------|----------|-------|---------|
| patch_sweep     | 22.72 s  | 17.17 s | 1.32x |
| chord_attack    |  9.74 s  |  6.70 s | 1.45x |
| dense_play      | 23.86 s  | 16.84 s | 1.42x |

A macOS/arm64 profile applies to the iOS/arm64 build: clang profiles key on
function names and counter indices, not on the target.
