#!/bin/sh
set -e

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <firmware-version>"
    echo "Example: $0 8.2"
    exit 1
fi

VERSION="$1"

if [ "${#VERSION}" -gt 10 ]; then
    echo "Firmware version is too long (maximum 10 characters)."
    exit 1
fi

mkdir -p compiled-firmware

echo "Building TrueMDC Gen${VERSION} with Docker..."

docker build -t uvk5 .

docker run --rm \
    -v "$(pwd)/compiled-firmware:/app/compiled-firmware" \
    uvk5 /bin/bash -c "cd /app && \
        make clean && \
        make build \
            FIRMWARE_VERSION='${VERSION}' \
            ENABLE_FMRADIO=1 \
            ENABLE_SPECTRUM=1 \
            ENABLE_MDC1200=1 \
            ENABLE_MDC1200_EDIT=1 \
            ENABLE_MDC1200_CONTACT=1 && \
        cp firmware.bin compiled-firmware/TrueMDC.Gen${VERSION}.bin && \
        cp archive/TrueMDC.Gen${VERSION}.packed.bin compiled-firmware/"

echo
echo "Build complete:"
echo "  compiled-firmware/TrueMDC.Gen${VERSION}.bin"
echo "  compiled-firmware/TrueMDC.Gen${VERSION}.packed.bin"
