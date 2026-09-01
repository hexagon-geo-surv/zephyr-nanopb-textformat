# nanopb_textformat -- common tasks. See README.md for what this library is.
#
# Every recipe runs from the repo root with ZEPHYR_BASE already exported, so
# there is nothing to activate first. `west` comes from the pyenv virtualenv
# named in .python-version.

board := "qemu_x86"

ws := justfile_directory() / "ws"
export ZEPHYR_BASE := justfile_directory() / "ws" / "zephyr"

# List the available recipes.
default:
    @just --list --unsorted

# Build the sample for BOARD.
build board=board:
    cd {{ ws }} && west build -p always -b {{ board }} -d build-sample ../samples/textformat

# Build and run the sample under QEMU. Gives you a shell: try `cfg examples`.
# Quit with Ctrl-A X.
run board=board: (build board)
    cd {{ ws }} && west build -d build-sample -t run

# The full gate: 4 configurations x 2 boards. Run before committing.
test:
    cd {{ ws }} && west twister -T ../tests -p mps2/an385 -p qemu_x86 --inline-logs

# One board only, for a faster loop.
test-fast board=board:
    cd {{ ws }} && west twister -T ../tests -p {{ board }} --inline-logs

# One configuration: core | nofloat | no64 | shallow.
test-one config board=board:
    cd {{ ws }} && west twister -T ../tests -p {{ board }} -s nanopb_textformat.{{ config }}

# Build the tests and run them directly, skipping twister. Fastest inner loop.
check board=board:
    cd {{ ws }} && west build -p always -b {{ board }} -d build-tests ../tests/textformat
    cd {{ ws }} && west build -d build-tests -t run

# Stack usage per function. pb_tf_parse and pb_tf_print must each stay one flat frame.
stack:
    cd {{ ws }} && west build -p always -b mps2/an385 -d build-measure \
        ../samples/textformat -- -DEXTRA_CFLAGS=-fstack-usage
    @cat {{ ws }}/build-measure/modules/nanopb_textformat/CMakeFiles/*/src/*.su \
        | awk -F'\t' '{split($1,a,":"); printf "%-28s %6s\n", a[4], $2}' | sort -k2 -rn

# Code size and RAM. data and bss must both stay 0.
#
# `size` comes from whichever toolchain the build used, read out of its own
# CMakeCache, so this does not depend on where the SDK is installed.
size:
    cd {{ ws }} && west build -p always -b mps2/an385 -d build-measure ../samples/textformat
    @sz=$(sed -n 's|^CMAKE_C_COMPILER:.*=\(.*\)-gcc$|\1-size|p' \
            {{ ws }}/build-measure/CMakeCache.txt); \
     test -x "$sz" || sz=size; \
     find {{ ws }}/build-measure -name 'nanopb_textformat_*.c.obj' -exec "$sz" {} +

# Bootstrap or re-bootstrap the in-repo west workspace (~600 MB fetch).
bootstrap:
    west init -l ws/manifest
    cd {{ ws }} && west update --narrow --fetch-opt=--depth=1
    cd {{ ws }} && west packages pip --install

# Remove build directories, keeping the fetched workspace.
clean:
    -rm -rf {{ ws }}/build-sample {{ ws }}/build-tests {{ ws }}/build-measure {{ ws }}/twister-out*
