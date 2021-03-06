/*
 * Copyright (c) 2017 Cesanta Software Limited
 * All rights reserved
 */

#ifndef MJS_DATAVIEW_H_
#define MJS_DATAVIEW_H_

/*
 * Functions for memory introspection.
 * These are supposed to be FFI-ed and used from the JS environment.
 */

void *mjs_mem_get_ptr(void *base, int offset);
void mjs_mem_set_ptr(void *ptr, void *val);
double mjs_mem_get_dbl(void *ptr);
void mjs_mem_set_dbl(void *ptr, double val);
unsigned mjs_mem_get_uint(void *ptr, int size, int bigendian);
void mjs_mem_set_uint(void *ptr, unsigned val, int size, int bigendian);
int mjs_mem_get_int(void *ptr, int size, int bigendian);
void mjs_mem_set_int(void *ptr, int val, int size, int bigendian);

#endif /* MJS_DATAVIEW_H_ */
