/* bm_vsync.h -- see bm_vsync.c for why the counter is driven rather than the
 * wait patched. */
#ifndef BM_VSYNC_H
#define BM_VSYNC_H
#include <stdint.h>
int      bm_vsync_init(void);   /* after libunity is mapped; 0 on success */
void     bm_vsync_tick(void);   /* one vsync; called by the pump thread */
int      bm_vsync_start(void);  /* start the pump -- REQUIRED, see bm_vsync.c */
void     bm_vsync_stop(void);
uint64_t bm_vsync_frames(void);
#endif
