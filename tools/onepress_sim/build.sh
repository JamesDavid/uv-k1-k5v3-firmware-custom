#!/usr/bin/env bash
# Build the PC simulator of the OnePress apps (see opsim.c), from anywhere:
#   tools/onepress_sim/build.sh && build/opsim model/onepress-r.bin "4 6 6 3 s"
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
OP="-DOP_EXT_MODEL -DOP_NO_NN -DOP_NO_QWERTY -DOP_NO_CJK -DOP_NO_ADDRESS -DOP_MAX_BEAM=3 -DOP_MAX_SLOTS=24 -DOP_MAX_KEYS=72"
I="-Itools/onepress_sim -IApp"
APP="-std=gnu11 -O1 -fno-builtin -Wno-attributes"
gcc $APP $I -Dapp_main=onepress_main -Dmemset=op_memset -Dmemcpy=op_memcpy -c App/apps/onepress/onepress_app.c -o build/sim_onepress.o
gcc $APP $I -Dapp_main=opaprs_main -Dmemset=aprs_memset -Dmemcpy=aprs_memcpy -DSYST_VAL='sim_systval()' -DSYST_LOAD=479999u \
    -include stdint.h -include tools/onepress_sim/systval.h -c App/apps/opaprs/opaprs_app.c -o build/sim_opaprs.o
gcc $APP $I -Dapp_main=opinbox_main -Dmemset=inbox_memset -c App/apps/opinbox/opinbox_app.c -o build/sim_opinbox.o
gcc $APP $I -Dapp_main=oploc_main -Dmemset=loc_memset -c App/apps/oploc/oploc_app.c -o build/sim_oploc.o
gcc -std=gnu11 -O2 -Wall $I $OP tools/onepress_sim/opsim.c App/app/onepress_svc.c App/onepress/onepress.c App/onepress/aprs_rx.c App/onepress/aprs_tx.c \
    build/sim_onepress.o build/sim_opaprs.o build/sim_opinbox.o build/sim_oploc.o -o build/opsim
