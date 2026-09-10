/* SPDX-License-Identifier: GPL-2.0 */
/* Kbuild needs distinct names for the composite module and its C object.
 * Keep the authored backend in vmctx.c so existing source references remain
 * stable; dependency generation tracks that file and all of its headers. */
#include "vmctx.c"
