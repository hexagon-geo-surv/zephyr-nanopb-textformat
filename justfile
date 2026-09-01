# nanopb_textformat -- common tasks. See README.md for what this library is.
#
# Every recipe runs from the repo root with ZEPHYR_BASE already exported, so
# there is nothing to activate first. `west` comes from the pyenv virtualenv
# named in .python-version.
#
# Note on the test recipes: `test` covers the cross targets, `test-native`
# covers native_sim. Both matter. native_sim is the only target that compiles
# against a *host* libc, at -std=c17, and it catches a class of portability bug
# the cross targets cannot -- v0.1.0 shipped one (strnlen, POSIX not ISO C).

board := "qemu_x86"

ws := justfile_directory() / "ws"
export ZEPHYR_BASE := justfile_directory() / "ws" / "zephyr"

# List the available recipes.
default:
    @just --list --unsorted

# Build the sample for BOARD.
build board=board:
    cd {{ ws }} && west build -p always -b {{ board }} -d build-sample ../samples/textformat

# Build and run the sample under QEMU; gives a shell, Ctrl-A X to quit.
run board=board: (build board)
    cd {{ ws }} && west build -d build-sample -t run

# The cross gate: 4 configurations x 2 boards. Run before committing.
test:
    cd {{ ws }} && west twister -T ../tests -p mps2/an385 -p qemu_x86 --inline-logs

# The host-libc gate: native_sim in Docker. Needs `test-native-image` once.
test-native platform="native_sim":
    docker run --rm -v {{ justfile_directory() }}:/work nptf-native sh -c '\
        export ZEPHYR_BASE=/work/ws/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host && \
        cd /work/ws && \
        west twister -T ../tests -p {{ platform }} --inline-logs -O /tmp/tw'

# Build the container test-native uses. On aarch64 pass native_sim/native/64.
test-native-image:
    -docker rm -f nptf-build
    docker run --name nptf-build -v {{ justfile_directory() }}:/work \
        python:3.12-slim-bookworm sh -c '\
            set -e; export DEBIAN_FRONTEND=noninteractive; apt-get update -qq; \
            apt-get install -y -qq --no-install-recommends build-essential \
                gcc-multilib g++-multilib cmake ninja-build git \
                device-tree-compiler ca-certificates >/dev/null || \
            apt-get install -y -qq --no-install-recommends build-essential \
                cmake ninja-build git device-tree-compiler ca-certificates >/dev/null; \
            pip install --no-cache-dir -q \
                -r /work/ws/zephyr/scripts/requirements-base.txt \
                -r /work/ws/zephyr/scripts/requirements-run-test.txt \
                west protobuf grpcio-tools junitparser natsort pytest'
    docker commit nptf-build nptf-native
    docker rm nptf-build

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

# Stack per function; pb_tf_parse and pb_tf_print must each stay one flat frame.
stack:
    cd {{ ws }} && west build -p always -b mps2/an385 -d build-measure \
        ../samples/textformat -- -DEXTRA_CFLAGS=-fstack-usage
    @cat {{ ws }}/build-measure/modules/nanopb_textformat/CMakeFiles/*/src/*.su \
        | awk -F'\t' '{split($1,a,":"); printf "%-28s %6s\n", a[4], $2}' | sort -k2 -rn

# Code size and RAM; data and bss must both stay 0.
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
