#!/bin/bash

set -xeu -o pipefail

REPO_PATH="$(pwd)/../../"
DEST="${1}"

echo "DEST=${DEST}"
touch "${DEST}"

TMP_IID_FILE=$(mktemp)

GUEST_IMAGE_TAG=guestassets
docker build --progress=plain \
       -t $GUEST_IMAGE_TAG \
       ${REPO_PATH}/examples/benchmarks/image/

for TARGET in $(ls -1 ${REPO_PATH}examples/benchmarks/targets/) ; do
    TARGET_DIR="${REPO_PATH}examples/benchmarks/targets/${TARGET}/"

    ( cd "${REPO_PATH}" && git apply "${TARGET_DIR}/diff" )

    TARGET_CPU=$(jq -r '.cpu' "${TARGET_DIR}/spec.json")
    MEMORY64=$(jq -r '.memory64' "${TARGET_DIR}/spec.json")
    EXTRA_CONFIGURE_FLAGS=$(jq -r '.extra_configure_args' "${TARGET_DIR}/spec.json")

    docker build --progress=plain -t benchenv \
           --build-context qemusrc=${REPO_PATH} \
           --build-arg TARGET_CPU="${TARGET_CPU}" \
           --build-arg WASM64_MEMORY64="${MEMORY64}" \
           --build-arg GUEST_IMAGE_TAG="${GUEST_IMAGE_TAG}" \
           --build-arg EXTRA_CONFIGURE_FLAGS="${EXTRA_CONFIGURE_FLAGS}" \
           - < ${REPO_PATH}/examples/benchmarks/emsdk-wasm-cross.docker

    ( docker run --name testrun --rm benchenv \
             /emsdk/node/24.7.0_64bit/bin/node /qemu/examples/benchmarks/run-emscripten.mjs \
             --preload ./load.js qemu-system-x86_64.js -- \
             -nographic \
             -m 512M \
             -L /pack/ \
             -nic none \
             -drive if=virtio,format=raw,file=/pack/rootfs.bin \
             -kernel /pack/Image \
             -append "earlyprintk=ttyS0 console=ttyS0 root=/dev/vda rootwait ro loglevel=7 init=/run.sh" || true ) | \
        while read -r LINE; do
            echo "$LINE"
            if [[ "$LINE" == *'>>>>>*'* ]]; then
                STARTTIME=$(date +%s%N)
            elif [[ "$LINE" == *'<<<<<*'* ]]; then
                ENDTIME=$(date +%s%N)
                DURATION_NS=$((ENDTIME - STARTTIME))
                echo "Duration: $((DURATION_NS / 1000000)) ms"
                echo -n "${TARGET}," >> "${DEST}"
                echo -n "${TARGET_CPU}," >> "${DEST}"
                echo -n "${MEMORY64}," >> "${DEST}"
                echo -n "${EXTRA_CONFIGURE_FLAGS}," >> "${DEST}"
                echo -n "${DURATION_NS}" >> "${DEST}"
                echo ""  >> "${DEST}"
                docker kill testrun
            fi
        done
    ( cd "${REPO_PATH}" && git apply -R "${TARGET_DIR}/diff" )
done

echo "========================"
cat "${DEST}"

rm "$TMP_IID_FILE"
