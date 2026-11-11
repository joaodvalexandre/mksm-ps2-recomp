#!/bin/bash
# Runs every check of tools/vu1gen in one go (development; Git Bash). Build first (tools/vu1gen/devbuild.cmd).
#
#   tools/vu1gen/selftest/check_all.sh [--quick]
#
# --quick limits the passes over the captures to the first 1000 launches of each file.
#
# Environment (the defaults are the places of the main repository layout):
#   MKSM_VU1_BUILD       the development build tree                      default <repository>/PS2Recomp/out/build-vu1
#   MKSM_VU1_ELF         the game ELF                                    default <repository>/game/SLES_535.24
#   MKSM_VU1_CAPTURES    the folder with launches_<hash>.bin             default <repository>/logs/vu1cap
#   MKSM_VU1_GAME_BUILD  the game's quick build tree (compile flags)     default <repository>/PS2Recomp/out/build-quick
#   MKSM_VU1_STUDY       the folder with vudis.py; without it the listing comparison is skipped
#
# The outputs go to <build tree>/check/ (they quote the game's microcode: never into the repository). The exit code
# is the number of checks that failed.
REPO=$(cd "$(dirname "$0")/../../.." && (pwd -W 2>/dev/null || pwd))
BUILD=${MKSM_VU1_BUILD:-$REPO/PS2Recomp/out/build-vu1}
ELF=${MKSM_VU1_ELF:-$REPO/game/SLES_535.24}
CAPTURES=${MKSM_VU1_CAPTURES:-$REPO/logs/vu1cap}
GAME_BUILD=${MKSM_VU1_GAME_BUILD:-$REPO/PS2Recomp/out/build-quick}
STUDY=${MKSM_VU1_STUDY:-}
IMAGES=$REPO/config/vu1_images.txt
TOOLS=$BUILD/vu1gen
OUT=$BUILD/check
LIMIT=
[ "$1" = "--quick" ] && LIMIT="--limit 1000"

mkdir -p "$OUT"
failed=0
skipped=0

# run <name> <command...>: the output goes to <name>.txt, the last line of it is shown.
run() {
    local name=$1
    shift
    if "$@" > "$OUT/$name.txt" 2>&1; then
        printf 'PASS  %-18s %s\n' "$name" "$(tail -n 1 "$OUT/$name.txt" | cut -c1-150)"
    else
        printf 'FAIL  %-18s see %s\n' "$name" "$OUT/$name.txt"
        failed=$((failed + 1))
    fi
}

skip() {
    printf 'SKIP  %-18s %s\n' "$1" "$2"
    skipped=$((skipped + 1))
}

listing_check() {
    "$TOOLS/vu1gen.exe" --elf "$ELF" --images "$IMAGES" --listing "$OUT/listing-cpp" || return 1
    python -I "$REPO/tools/vu1gen/selftest/vudis_listing.py" "$STUDY" "$ELF" "$OUT/listing-py" || return 1
    diff -r "$OUT/listing-py" "$OUT/listing-cpp" || return 1
    echo "the listings of vu1gen and vudis.py are identical ($(cat "$OUT"/listing-cpp/*.txt | wc -l) lines)"
}

# The oracle pass over the captures. Its exit code says nothing about the two checks of the oracle itself, so their
# numbers are taken from the report: an MSCNT entered by execute() must give what resume() gives, every time. (The
# sanity count is shown only: a pair of consecutive records differs when another program ran in between.)
captures_check() {
    local report=$OUT/captures-report.txt
    if ! "$TOOLS/vu1difftest.exe" --captures "$CAPTURES" --images "$IMAGES" $LIMIT > "$report" 2>&1; then
        cat "$report"
        return 1
    fi
    cat "$report"
    local all pairs equal resumed resumedEqual reset resetEqual
    all=$(sed -n '/^== all images ==/,$p' "$report")
    pairs=$(echo "$all" | sed -n 's/.*consecutive records compared *\([0-9]*\).*/\1/p')
    equal=$(echo "$all" | sed -n 's/.*equal, compared fields *\([0-9]*\).*/\1/p')
    read -r resumed resumedEqual <<< "$(echo "$all" | sed -n 's/.*ran the launch before *\([0-9]*\) launches, \([0-9]*\) equal.*/\1 \2/p')"
    read -r reset resetEqual <<< "$(echo "$all" | sed -n 's/.*(scheduler reset) *\([0-9]*\) launches, \([0-9]*\) equal.*/\1 \2/p')"
    echo "sanity $equal of $pairs consecutive records; MSCNT by execute() = by resume() in $resumedEqual of $resumed and $resetEqual of $reset launches"
    [ -n "$resumed" ] && [ "$resumed" = "$resumedEqual" ] && [ "$reset" = "$resetEqual" ]
}

run unit-tests "$TOOLS/vu1n_tests.exe"
run images "$TOOLS/vu1gen.exe" --elf "$ELF" --images "$IMAGES"
if [ -n "$STUDY" ]; then
    run listing listing_check
else
    skip listing "MKSM_VU1_STUDY is not set"
fi
if [ -f "$GAME_BUILD/compile_commands.json" ]; then
    run oracle-flags python -I "$REPO/tools/vu1gen/selftest/compare_compile_commands.py" "$BUILD" "$GAME_BUILD"
else
    skip oracle-flags "no compile_commands.json in $GAME_BUILD"
fi
run decoder "$TOOLS/vu1difftest.exe" --decode-check --captures "$CAPTURES"
run kick "$TOOLS/vu1difftest.exe" --kick-check
run captures captures_check
run self "$TOOLS/vu1difftest.exe" --captures "$CAPTURES" --images "$IMAGES" --self $LIMIT
run self-fault "$TOOLS/vu1difftest.exe" --captures "$CAPTURES" --images "$IMAGES" --self-fault 7 $LIMIT
run coverage "$TOOLS/vu1difftest.exe" --captures "$CAPTURES" --images "$IMAGES" --coverage $LIMIT

echo "$failed failed, $skipped skipped; outputs in $OUT"
exit $failed
