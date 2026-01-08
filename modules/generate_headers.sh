#!/bin/env bash

for YANG_FILE in *.yang; do
    # generate HEX
    HEX=$(echo "$(cat "$YANG_FILE")" | xxd -i)

    # generate array name
    ARRAY_NAME="$(echo "$YANG_FILE" | tr .@- _)"

    # generate header file name with the revision
    HEADER_FILE="$(echo "${YANG_FILE%?????}").h"

    # print into a C header file
    echo -e "char ${ARRAY_NAME}[] = {\n$HEX, 0x00\n};" > "$HEADER_FILE"
done
