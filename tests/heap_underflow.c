/* iso_alloc heap_underflow.c
 * Copyright 2021 - chris.rohlf@gmail.com */

#include "iso_alloc.h"
#include "iso_alloc_internal.h"

int main(int argc, char *argv[]) {
    uint8_t *p = NULL;

    for(int32_t i = 0; i < 128; i++) {
        p = (uint8_t *) iso_alloc(32);
        iso_free(p);
    }

    p = (uint8_t *) iso_alloc(32);
    memset(p - 65535, 0x42, 65535);
    iso_free(p);
    iso_verify_zones();

    return OK;
}
