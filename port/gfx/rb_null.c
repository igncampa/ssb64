// Headless backend: the interpreter runs in full, nothing is drawn.
#include "gfx.h"
void rb_begin_frame(void) {}
void rb_end_frame(void) {}
void rb_clear_depth(void) {}
void rb_draw(const RbState *st, const RbVertex *v, int n) {}
void port_set_canvas_size(int w, int h) { (void)w; (void)h; }
