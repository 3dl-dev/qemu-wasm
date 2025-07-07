#!/bin/bash

set -euo pipefail

BUILD_IMAGE_NAME=testbuildqemu
BUILD_CONTAINER_NAME=testbuildqemu
REPO_PATH="$(pwd)/../../"

ALL_ARCHS=(
    "x86_64"
    "riscv64"
    "aarch64"
    "arm"
    "s390x"
)

ARCHS=("${ALL_ARCHS[@]}")
if [[ $# -gt 0 ]]; then
    ARCHS=($(printf "%s\n" "${ALL_ARCHS[@]}" | grep -E "$1"))
fi

BROWSERS=(
    "node"
    "chrome"
    "firefox"
    "edge"
)

TESTS=($(ls -1 targets))

echo "Tests: ${TESTS[@]}"
echo "Archs: ${ARCHS[@]}"
echo "Browsers: ${BROWSERS[@]}"

RES=()
FAIL="false"

for TEST in "${TESTS[@]}" ; do
    for ARCH in "${ARCHS[@]}" ; do
        TARGET_WASM=wasm32
        MEMORY64=(0)
        if [ "${ARCH}" != "arm" ] ; then
            TARGET_WASM=wasm64
            MEMORY64=(1 2)
        fi
        for WASM64_MEMORY64 in "${MEMORY64[@]}" ; do
            if [[ -f targets/${TEST}/arch ]] ; then
                if ! cat targets/${TEST}/arch | grep ${ARCH} ; then
                    echo "Test ${TEST} doesn't supported this arch ${ARCH}; skipping..."
                    continue
                fi
            fi
            docker build --progress=plain -t buildbase --build-arg TARGET_CPU=${TARGET_WASM} --build-arg WASM64_MEMORY64=${WASM64_MEMORY64} - < ${REPO_PATH}/tests/docker/dockerfiles/emsdk-wasm-cross.docker
            cat <<EOF | docker build --progress=plain -t ${BUILD_IMAGE_NAME} -
FROM buildbase
WORKDIR /builddeps/
RUN npm i xterm-pty@v0.10.1
RUN cp /builddeps/node_modules/xterm-pty/emscripten-pty.js /builddeps/target/lib/libemscripten-pty.js
ENV EMCC_CFLAGS="-L/builddeps/target/lib/ -lemscripten-pty.js -Wno-unused-command-line-argument"
WORKDIR /build/
EOF
            WASM64_MEMORY64_FLAG=""
            if [ "${WASM64_MEMORY64}" != "2" ] ; then
                WASM64_MEMORY64_FLAG="--enable-wasm64-32bit-address-limit"
            fi

            # Compile QEMU with xterm-pty (for the browser tests)
            docker build --progress=plain -t htdocsassets \
                   --build-arg TEST_TARGET_ARCH=${ARCH} \
                   --build-arg QEMU_BUILD_BASE=${BUILD_IMAGE_NAME} \
                   --build-arg TARGET_WASM=${TARGET_WASM} \
                   --build-arg WASM64_MEMORY64_FLAG=${WASM64_MEMORY64_FLAG} \
                   --build-context qemusrc=${REPO_PATH} \
                   targets/${TEST}/image/

            # Compile QEMU with xterm-pty (for the node tests)
            docker build --progress=plain -t nodeassets \
                   --build-arg TEST_TARGET_ARCH=${ARCH} \
                   --build-arg QEMU_BUILD_BASE=buildbase \
                   --build-arg TARGET_WASM=${TARGET_WASM} \
                   --build-arg WASM64_MEMORY64_FLAG=${WASM64_MEMORY64_FLAG} \
                   --build-context qemusrc=${REPO_PATH} \
                   targets/${TEST}/image/

            docker build --progress=plain -t test-page -f ./Dockerfile.testpage .
            docker build --progress=plain -t test-node-chrome -f ./Dockerfile.node-chrome .
            docker build --progress=plain -t test-node-firefox -f ./Dockerfile.node-firefox .
            docker build --progress=plain -t test-node-edge -f ./Dockerfile.node-edge .

            docker compose up -d --force-recreate --build
            docker cp "$(pwd)/targets/${TEST}/run.exp" tests-runner-1:/
            jq -r ' "Module[\"arguments\"] = \(. | @json);" ' ./targets/${TEST}/image/arguments-$ARCH.json |
                docker exec -i tests-testpage-1 /bin/bash -c "cat > /usr/local/apache2/htdocs/module.js"

            cat <<EOF | docker build --progress=plain -t nodetest -
FROM nodeassets AS assets
FROM node:24
RUN apt-get update && apt-get install -y expect
COPY --from=assets / /assets/
WORKDIR /assets/
EOF
            docker kill nodetest && docker rm nodetest || true ; sleep 1
            docker run --rm --init -d --name nodetest nodetest sleep infinity
            docker cp "${REPO_PATH}/scripts/run-emscripten.mjs" nodetest:/
            docker cp "$(pwd)/targets/${TEST}/run.exp" nodetest:/

            for BROWSER in "${BROWSERS[@]}" ; do
                NAME="test:${TEST} arch:${ARCH} browser:${BROWSER} memory64:${WASM64_MEMORY64}"
                echo "Running test: ${NAME}"
                TEST_COMMAND=
                if [ "$BROWSER" == "node" ] ; then
                    TEST_COMMAND="docker exec nodetest /bin/bash -c \"expect /run.exp node /run-emscripten.mjs --preload load.mjs -- out.js $(jq -r '@sh' ./targets/${TEST}/image/arguments-$ARCH.json)\""
                else
                    TEST_COMMAND="docker exec -e TARGET_BROWSER=${BROWSER} tests-runner-1 expect /run.exp python3 /run.py"
                fi
                if /bin/bash -c "$TEST_COMMAND" ; then
                    RES+=("OK: ${NAME}")
                else
                    RES+=("NG: ${NAME}")
                    FAIL="true"
                fi
            done
        done
    done
done

docker compose down -v
printf "%s\n" "${RES[@]}"

if [[ "${FAIL}" == "true" ]] ; then
    exit 1
fi
