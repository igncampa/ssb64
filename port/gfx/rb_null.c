// Headless backend: the interpreter runs in full, nothing is drawn.
#include "gfx.h"
void rb_begin_frame(void) {}
void rb_clear_depth(void) {}
void rb_draw(const RbState *st, const RbVertex *v, int n) {}
