/*
 * Copyright (c) 2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef MJS_FFI_PUBLIC_H_
#define MJS_FFI_PUBLIC_H_

#include "mjs/src/mjs_core_public.h"

typedef void *(mjs_ffi_resolver_t)(void *handle, const char *symbol);

void mjs_set_ffi_resolver(struct mjs *mjs, mjs_ffi_resolver_t *dlsym);

#endif /* MJS_FFI_PUBLIC_H_ */
