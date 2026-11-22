#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --build picoquic/build --target picoquic-test picohttp-core -j 4
cc -O2 -Wall -Wextra -I picoquic/picoquic -I picoquic/picoquictest -I picoquic/picohttp -I picotls/include -c exp/picoquic_main_budget_exp.c -o exp/picoquic_main_budget_exp.o
cc -o exp/picoquic_main_budget_exp exp/picoquic_main_budget_exp.o \
    -Wl,--start-group picoquic/build/libpicoquic-test.a picoquic/build/libpicoquic-log.a picoquic/build/libpicohttp-core.a picoquic/build/libpicoquic-core.a \
    picotls/libpicotls-core.a picotls/libpicotls-openssl.a picotls/libpicotls-minicrypto.a -Wl,--end-group -lssl -lcrypto -lm -lpthread -ldl
