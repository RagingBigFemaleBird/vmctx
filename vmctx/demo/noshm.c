/* Hide the MIT-SHM extension from an Xlib/xcb client: a vmctx guest cannot
 * share memory with the X server on the source (a foreign process touching a
 * context object through its own mm is the unhooked sharing class). With
 * these answering "no", GTK/cairo fall back to XPutImage over the socket. */
int XShmQueryExtension(void *dpy) { (void)dpy; return 0; }
int XShmQueryVersion(void *dpy, int *maj, int *min, int *pix) { (void)dpy; (void)maj; (void)min; (void)pix; return 0; }
void *xcb_shm_query_version_reply(void *c, unsigned int cookie, void **e) { (void)c; (void)cookie; if (e) *e = 0; return 0; }
