// Background gate. While visionOS has the app backgrounded (immersive space still open) Metal refuses
// every command buffer (kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted) and dumps a
// gpu-hang report per failure. Like a Quest runtime blocking the render thread when the app is not
// visible, the guest's frame-submission paths call kl_bg_wait() BEFORE committing GPU work and sit
// there until the app is foreground again or the run is shutting down.
#ifndef KL_BG_H
#define KL_BG_H

// Swift, from the scene phase: 1 = background, 0 = foreground. Never blocks; repeats are ignored.
void kl_bg_set(int background);
// Release every waiter for good (the run is ending; a join on a blocked guest thread must not hang).
// A later kl_bg_set(1) no longer blocks.
void kl_bg_shutdown(void);
// 1 while backgrounded and not shutting down.
int  kl_bg_active(void);
// Guest submit paths only (never the main/UI thread): returns at once in the foreground.
void kl_bg_wait(void);

#endif
