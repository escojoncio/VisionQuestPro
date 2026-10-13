// the main-thread queue and the frame clock: Handler/Looper,
// MessageQueue, Message, HandlerThread, Choreographer, Process
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include "kl_jni_int.h"

// ---- UI thread queue ----
// runOnUiThread posts to the main looper and returns; it does not run the
// Runnable inline unless already on that thread. Queuing is therefore the
// faithful behaviour, not a shortcut.
//
// kl_jni_drain_ui_tasks() runs them, which is the host->guest direction and the
// only one in this file: every other call here answers something the guest
// started. The delay is recorded but not honoured — a posted task runs at the
// next drain regardless of its delay, which is wrong in the same way a
// zero-latency looper is wrong, and has not mattered yet.
// A queue entry is either a Runnable to run or a Message to deliver to its
// target Handler's callback. One queue, because on Android both land on the same
// main looper and ordering between them is observable.
klj_ui_task g_ui_tasks[KLJ_MAX_UI_TASKS];
unsigned g_ui_task_n;

unsigned kl_jni_pending_ui_tasks(void) { return g_ui_task_n; }

// One queue behind both posting routes — runOnUiThread and Handler.post* target
// the same main-thread looper on Android, so splitting them would only hide half
// the backlog from kl_jni_pending_ui_tasks().
static void klj_ui_enqueue(const char *via, void *runnable, int64_t delay_ms) {
    if (runnable && g_ui_task_n < KLJ_MAX_UI_TASKS) {
        g_ui_tasks[g_ui_task_n].runnable = runnable;
        g_ui_tasks[g_ui_task_n].message  = NULL;
        g_ui_tasks[g_ui_task_n].delay_ms = delay_ms;
        g_ui_task_n++;
    }
    KLJ_LOG("%s: queued (+%lldms, %u pending)", via, (long long)delay_ms, g_ui_task_n);
}

static void klj_msg_enqueue(const char *via, void *message) {
    if (message && g_ui_task_n < KLJ_MAX_UI_TASKS) {
        g_ui_tasks[g_ui_task_n].runnable = NULL;
        g_ui_tasks[g_ui_task_n].message  = message;
        g_ui_tasks[g_ui_task_n].delay_ms = 0;
        g_ui_task_n++;
    }
    KLJ_LOG("%s: queued (%u pending)", via, g_ui_task_n);
}

// Forward-declared: the inline path below calls into the guest, and the JNIBridge
// machinery that does it lives further down beside the other host->guest calls.

// ...and here is the "unless already on that thread" half, which was missing and
// is not a nicety: `Activity.runOnUiThread` runs the action IMMEDIATELY and
// inline when the caller is already the UI thread, and a guest is allowed to
// depend on that having happened by the time the call returns.
//
// BONELAB does. Its `gles-api-check` warning is posted this way and then WAITED
// ON — the poster blocks on a condition variable the dialog's own callback
// signals — so queuing it deadlocked the process against itself: the only
// thread that drains the queue was the thread inside the wait
// (`__psynch_cvwait` under libunity, with the task still pending). Nothing in
// the log named a dialog; it read as a hung engine.
//
// "Am I the UI thread" is answered by WHO DRAINS THE QUEUE, which is a
// definition rather than a guess: the UI thread is the thread that runs posted
// work, so the thread inside kl_jni_drain_ui_tasks() is that thread and no
// other. A recorded id from the driver would be a second opinion about the same
// fact, and the two would eventually differ (they already do across drivers —
// kl_slink prepares a looper on its activity thread, the Unity path never
// does, so a looper test answers false for exactly the thread that matters).
//
// A thread that has never drained still queues, which is what Android does for
// a worker thread.
 pthread_t g_ui_thread;
 int       g_ui_thread_known;

static int klj_on_ui_thread(void) {
    return (g_ui_thread_known && pthread_equal(g_ui_thread, pthread_self()))
        || kl_ndk_thread_has_looper();
}

// Mark the CALLING thread as the activity/UI thread and give it a native looper.
// Android's main thread always has one (Looper.prepareMainLooper before onCreate),
// and the Unity path never prepared it here — so newer Unity guests failed "Couldn't retrieve native ALooper for UI thread" and spun
// on graphics-context setup until SIGILL. The driver calls this on the thread
// that runs UnityPlayer.initJni, before Unity's native code first asks.
void kl_jni_mark_ui_thread(void) {
    g_ui_thread = pthread_self();
    g_ui_thread_known = 1;
    kl_ndk_prepare_looper();
}

static klj_val klj_Activity_runOnUiThread(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    void *r = n > 0 ? a[0].l : NULL;
    if (r && klj_on_ui_thread()) {
        KLJ_LOG("runOnUiThread: already on the UI thread — running it inline");
        klj_proxy_invoke(r, "java/lang/Runnable", "run", "()V", NULL);
        return (klj_val){0};
    }
    klj_ui_enqueue("runOnUiThread", r, 0);
    return (klj_val){0};
}

// The main Looper is an identity, not a mechanism: Unity holds it to build a
// Handler and to ask whether it is already on that thread.
static klj_val klj_Looper_getMainLooper(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *main_looper;
    return klj_singleton("android/os/Looper", &main_looper);
}

// Looper.myLooper() — the CALLING thread's looper, or null if it has none.
// The distinction is the whole point of the call: a guest asks it to find out
// whether it may post work from here, and a runtime that always answers
// non-null tells every worker thread it is a UI thread.
//
// The native side already knows: ALooper_forThread() is the same question, and
// kl_ndk_prepare_looper is what puts one on the thread that runs the activity.
// So this defers to that rather than keeping a second idea of which threads are
// loopered — two answers to one question is how they come to disagree.
//
// The looper it names is the main one, which is right for the thread we prepare
// and would be wrong for a HandlerThread asking about itself. Nothing does that
// yet; when something does, this needs the per-thread object, not the singleton.
// Looper.getQueue() — the MessageQueue behind a Looper. Like the Looper itself
// this is an identity rather than a mechanism: the queue is ours (klj_looper's
// ring, or the native looper's), and nothing has yet called a method ON the
// MessageQueue — the guest holds it, which is what addIdleHandler and
// isIdle would need and neither has been reached.
//
// So it is a singleton per Looper object, and the moment something does call a
// method on it, that method is where the real queue gets attached.
static klj_val klj_Looper_getQueue(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    static void *queue;
    (void)o;
    return klj_singleton("android/os/MessageQueue", &queue);
}

// Looper.prepare() — give THIS thread a looper. On Android it is the first half
// of the two-line idiom every worker thread that wants a message queue writes
// (prepare, then loop). Both halves are ours: the native looper is the same
// object, so this is kl_ndk_prepare_looper by another name.
//
// Android throws RuntimeException on a second prepare for the same thread, and
// kl_ALooper_prepare is idempotent instead. That divergence is deliberate: the
// throw exists to catch a programming error, and we have no exception to throw
// that the guest would survive.
static klj_val klj_Looper_prepare(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    kl_ndk_prepare_looper();
    return (klj_val){0};
}

static klj_val klj_Looper_myLooper(void *env, void *self, const klj_val *a, int n) {
    if (!kl_ndk_thread_has_looper()) return (klj_val){.l = NULL};
    return klj_Looper_getMainLooper(env, self, a, n);
}
// new Handler() binds to the calling thread's Looper; new Handler(looper) to the
// one given. We have a single queue, so the Looper is recorded and not acted on.
// The Callback form keeps the callback: sendToTarget() has to find it again, and
// it is the only thing that gives a Message any meaning.

static klj_val klj_Handler_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_handler *h = calloc(1, sizeof *h);
    if (h) {
        h->looper   = n > 0 ? a[0].l : NULL;
        h->callback = n > 1 ? a[1].l : NULL;
    }
    klj_as_object(obj)->data = h;
    if (h && h->callback) {
        klj_object *cb = klj_as_object(h->callback);
        KLJ_LOG("new Handler(looper, callback=%s)", cb ? cb->cls : "(untagged)");
    }
    return (klj_val){.l = obj};
}

 klj_handler *klj_as_handler(void *obj) {
    klj_object *o = klj_as_object(obj);
    return (o && strcmp(o->cls, "android/os/Handler") == 0) ? o->data : NULL;
}
// postDelayed returns whether the message made it into the queue — which it did.
// The delay is recorded rather than honoured; there is no clock driving this
// queue until something drains it.
static klj_val klj_Handler_postDelayed(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_ui_enqueue("Handler.postDelayed", n > 0 ? a[0].l : NULL,
                   n > 1 ? (int64_t)a[1].j : 0);
    return (klj_val){.j = 1};
}
static klj_val klj_Handler_post(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_ui_enqueue("Handler.post", n > 0 ? a[0].l : NULL, 0);
    return (klj_val){.j = 1};
}

// ---- HandlerThread loopers ----
//
// A HandlerThread is a real thread with a real message loop, and here that turns
// out to be load-bearing rather than a detail worth simplifying away.
//
// The first version of this treated a HandlerThread's Looper as the main one and
// its start() as a no-op, on the reasoning that everything drains through
// kl_jni_drain_ui_tasks() anyway. That is true for Runnables, and wrong for
// Messages: the guest sends a message and then *blocks* waiting for the handler to
// run, which only works if the loop is on another thread. With no such thread the
// main thread sat in __psynch_cvwait until the watchdog fired — a hang whose cause
// was two layers away from where it presented, exactly the failure mode a silent
// no-op produces.
//
// So a started HandlerThread gets a host thread and its own queue. The main looper
// keeps the host-driven drain, because there genuinely is no thread of ours
// running it.
#define KLJ_MAX_LOOPER_MSGS 64

// java.lang.Thread lifecycle state (AoT log 42: Unity's AndroidVideoMedia asked
// `Thread.isAlive()` on its HandlerThread while tearing a decoder down, and the
// call had no binding -> abort). Shared by every Thread-shaped object we hand
// out: a HandlerThread embeds one (its klj_looper carries it, so the Looper
// objects that share the klj_looper see the same thread), a plain
// java/lang/Thread made by NewObject owns one, and Thread.currentThread() makes
// one per host thread. `done` is broadcast when the thread's body returns, which
// is what join() and isAlive() answer from.
typedef struct klj_jthread {
    pthread_mutex_t mu;
    pthread_cond_t  done;
    pthread_t       thread;
    int             started, finished, is_current, interrupted, daemon, priority;
    uint64_t        tid;            // pthread_threadid_np of the running thread
    void           *runnable;       // java/lang/Thread(Runnable): pinned while held
    void           *self_obj;       // the Thread object, pinned while it runs
    char            name[64];
} klj_jthread;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    pthread_t       thread;
    int             running, started;
    int             drain_on_quit;  // quitSafely(): deliver what is queued, then stop
    void           *q[KLJ_MAX_LOOPER_MSGS];
    unsigned        head, tail, count;
    klj_jthread     th;             // the HandlerThread's Thread half
} klj_looper;

static void klj_jthread_init(klj_jthread *t, const char *name) {
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->done, NULL);
    t->priority = 5;                // Thread.NORM_PRIORITY
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
}

// Mark the thread's body as returned and wake every joiner.
static void klj_jthread_finish(klj_jthread *t) {
    pthread_mutex_lock(&t->mu);
    t->finished = 1;
    pthread_cond_broadcast(&t->done);
    pthread_mutex_unlock(&t->mu);
}


// Guest code runs on this thread, so kl_thread_init() before the first delivery is
// mandatory — without it the stack-protector prologue reads an
// empty TSD slot and the guest dies a long way from here.
static void *klj_looper_thread(void *arg) {
    klj_looper *lp = arg;
    kl_thread_init();
    {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        pthread_mutex_lock(&lp->th.mu); lp->th.tid = tid; pthread_mutex_unlock(&lp->th.mu);
        if (lp->th.name[0]) pthread_setname_np(lp->th.name);
    }
    pthread_mutex_lock(&lp->lock);
    for (;;) {
        // quit() stops at once and drops what is queued; quitSafely() delivers
        // what is already queued first (Android: messages already due).
        if (!lp->running && !(lp->drain_on_quit && lp->count)) break;
        if (!lp->count) { pthread_cond_wait(&lp->wake, &lp->lock); continue; }
        void *msg = lp->q[lp->head];
        lp->head  = (lp->head + 1) % KLJ_MAX_LOOPER_MSGS;
        lp->count--;
        pthread_mutex_unlock(&lp->lock);
        klj_deliver_message(msg);          // outside the lock: it runs guest code
        pthread_mutex_lock(&lp->lock);
    }
    pthread_mutex_unlock(&lp->lock);
    // The HandlerThread's run() has returned: isAlive() is now false and any
    // join() returns. Without this a guest that quits and joins (the standard
    // teardown) would block forever.
    klj_jthread_finish(&lp->th);
    KLJ_LOG("HandlerThread \"%s\" looper exited", lp->th.name[0] ? lp->th.name : "?");
    return NULL;
}

static void klj_looper_post(klj_looper *lp, void *message) {
    pthread_mutex_lock(&lp->lock);
    if (lp->count < KLJ_MAX_LOOPER_MSGS) {
        lp->q[lp->tail] = message;
        lp->tail = (lp->tail + 1) % KLJ_MAX_LOOPER_MSGS;
        lp->count++;
    } else {
        KLJ_LOG("looper queue full — message dropped");
    }
    pthread_cond_signal(&lp->wake);
    pthread_mutex_unlock(&lp->lock);
}

// A Looper object carries the klj_looper it belongs to, or NULL for the main one.
static klj_looper *klj_looper_of(void *looper_obj) {
    klj_object *o = klj_as_object(looper_obj);
    return (o && strcmp(o->cls, "android/os/Looper") == 0) ? o->data : NULL;
}

// Looper.quit() — stop the loop on the looper this object names. For a
// HandlerThread's looper that is a real thread to shut down; for the main one
// there is nothing to stop, because our main "loop" is the host's pump and it
// ends when the run does.
static void klj_mq_quit(void);
static void klj_looper_stop(klj_looper *lp, int safely) {
    pthread_mutex_lock(&lp->lock);
    lp->running = 0;
    lp->drain_on_quit = safely;
    pthread_cond_broadcast(&lp->wake);
    pthread_mutex_unlock(&lp->lock);
}
static klj_val klj_Looper_quit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_looper_of(self);
    if (!lp) {
        KLJ_LOG("Looper.quit() on the main looper — releasing anything blocked in "
                "MessageQueue.next()");
        klj_mq_quit();
        return (klj_val){0};
    }
    klj_looper_stop(lp, 0);
    return (klj_val){0};
}
// Looper.quitSafely() — same, but what is already queued is delivered first.
static klj_val klj_Looper_quitSafely(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_looper *lp = klj_looper_of(self);
    if (!lp) { klj_mq_quit(); return (klj_val){0}; }
    klj_looper_stop(lp, 1);
    return (klj_val){0};
}

// ---- android.os.MessageQueue.next() ----------------------------------------
//
// A blocking pop off the Looper's queue, and the first thing in this shim to
// call one. Who calls it is worth writing down, because it is not obvious and it
// is the answer to "how does the in-headset UI talk BACK to the guest".
//
// libvrlink_scene cannot subclass WebMessagePort$WebMessageCallback — it is all
// native, there is no Java of its own in the APK — so it never registers one:
// WebView::UIThread_SetupWebView passes a NULL callback and a Handler bound to
// the WebView thread's Looper (+0x149d1c). Then that thread runs its OWN loop
// (WebView::WebViewThread +0x14a9b0): queue.next(), and for each Message it
// reads the payload straight out of the Message's fields, skipping the three
// framework target classes it knows by name. It is intercepting the framework's
// own delivery instead of receiving it. Clever, and it means a page->native
// message here is a Message on this queue, not a callback we can invoke.
//
// Nothing posts one. Java Messages in this shim go to a HandlerThread's own
// queue (klj_looper above) and Runnables go through kl_jni_drain_ui_tasks, so
// this queue is genuinely, correctly empty — and an empty queue is a wait, not
// a NULL. NULL means "the looper quit", and returning it would tell the guest to
// tear its WebView thread down. So this blocks, exactly as Android's does, and
// says so once so that a future stall here has a name instead of being a silent
// parked thread.
#define KLJ_MQ_MAX 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_mq_wake = PTHREAD_COND_INITIALIZER;
static void     *g_mq_q[KLJ_MQ_MAX];
static unsigned  g_mq_head, g_mq_count;   // push goes at (head + count) % KLJ_MQ_MAX
static int       g_mq_quit, g_mq_said;

static void klj_mq_quit(void) {
    pthread_mutex_lock(&g_mq_lock);
    g_mq_quit = 1;
    pthread_cond_broadcast(&g_mq_wake);
    pthread_mutex_unlock(&g_mq_lock);
}

static klj_val klj_MessageQueue_next(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    pthread_mutex_lock(&g_mq_lock);
    for (;;) {
        if (g_mq_count) {
            void *msg = g_mq_q[g_mq_head];
            g_mq_head = (g_mq_head + 1) % KLJ_MQ_MAX;
            g_mq_count--;
            pthread_mutex_unlock(&g_mq_lock);
            return (klj_val){.l = msg};
        }
        if (g_mq_quit) {
            pthread_mutex_unlock(&g_mq_lock);
            return (klj_val){.l = NULL};
        }
        if (!g_mq_said) {
            g_mq_said = 1;
            KLJ_LOG("MessageQueue.next() — blocking on an empty queue; nothing "
                    "posts Java Messages here (this is the WebView's page->native "
                    "pump, and no page is talking)");
        }
        pthread_cond_wait(&g_mq_wake, &g_mq_lock);
    }
}


// ---- android.os.Message ----
//
// A Message is a Runnable's counterpart on the same queue: post() carries code to
// run, sendMessage() carries data for the target Handler's callback to interpret.
// Both end up on the main looper, so they share the queue below rather than
// getting a second one.
//
// What is actually behind this in Beat Saber is Unity's AudioVolumeHandler: it
// registers for volume-change notifications and turns each one into a Message
// whose handler calls the guest native onAudioVolumeChanged(int). Nothing on this
// side changes the volume, so no message is expected to be *sent* — obtainMessage
// is reached during setup regardless. That is why this is deliberately only as
// much machinery as the trace forces: the object and its fields, and no delivery
// path until something is proven to send one.

 klj_message *klj_as_message(void *obj) {
    klj_object *o = klj_as_object(obj);
    return (o && strcmp(o->cls, "android/os/Message") == 0) ? o->data : NULL;
}

// obtainMessage(what) — Android recycles these from a pool; we allocate, because
// the pool is an allocation optimisation and nothing observable depends on it.
static klj_val klj_Handler_obtainMessage(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_message *m = calloc(1, sizeof *m);
    if (!m) return (klj_val){.l = NULL};
    m->what   = n > 0 ? (int32_t)a[0].j : 0;
    m->target = self;                       // sendToTarget() needs to find it back
    void *obj = klj_new_object_data("android/os/Message", m);
    // Pinned because it is queued and outlives the frame that made it — and
    // taken OUT of that frame for the same reason. Both halves are needed: the
    // pin stops the frame pop retiring it early, and the frame no longer
    // listing it is what makes klj_deliver_message's retire the only one. A
    // frame entry that survives its object is how a pop retires whoever
    // recycled the slot, and the creating frame is on a different thread from
    // the delivery, so klj_frame_forget could never reach it from there.
    pthread_mutex_lock(&g_lock);
    klj_as_object(obj)->pinned  = 1;
    klj_as_object(obj)->destroy = free;   // obj/target are pool jobjects, not ours
    klj_frame_forget(klj_as_object(obj));
    pthread_mutex_unlock(&g_lock);

    // Message's members are public *fields*, not getters, so the handler reads
    // msg.what with GetIntField. Publishing them through the same per-object write
    // table the guest's own Set*Field uses means a read finds them by the ordinary
    // path — no instance-field special case, and a guest that writes one back gets
    // the write it expects.
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "what", "I", 'f'),
                    (klj_val){.j = (uint64_t)(int64_t)m->what});
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "arg1", "I", 'f'),
                    (klj_val){.j = 0});
    klj_field_store(obj, klj_want(klj_class_object("android/os/Message"), "arg2", "I", 'f'),
                    (klj_val){.j = 0});
    KLJ_LOG("Handler.obtainMessage(what=%d)", m->what);
    return (klj_val){.l = obj};
}

// sendToTarget() posts the message to the Handler it came from. Never delivered
// inline: Android returns immediately and the looper delivers later, and the
// sender here is frequently about to block waiting for that to happen on another
// thread — delivering inline would run the callback before the sender is ready
// for it, and on the wrong thread.
//
// Which queue depends on the target Handler's Looper. A HandlerThread's looper has
// a thread of its own to deliver on; the main looper does not, so its messages
// wait for the host's drain.
static klj_val klj_Message_sendToTarget(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_message *m = klj_as_message(self);
    if (!m) {
        KLJ_LOG("Message.sendToTarget() on something that is not a Message");
        return (klj_val){.j = 0};
    }
    klj_handler *h  = klj_as_handler(m->target);
    klj_looper  *lp = h ? klj_looper_of(h->looper) : NULL;
    if (lp && lp->started && !lp->running) {
        // Android: sending to a quit looper fails (the message is dropped and
        // sendMessage returns false). Queuing it would park it forever.
        KLJ_LOG("Message.sendToTarget(what=%d) -> HandlerThread looper has quit — dropped",
                m->what);
    } else if (lp && lp->started) {
        KLJ_LOG("Message.sendToTarget(what=%d) -> HandlerThread looper", m->what);
        klj_looper_post(lp, self);
    } else {
        klj_msg_enqueue("Message.sendToTarget", self);
    }
    return (klj_val){.j = 0};
}

// Message.getTarget() / Handler.getLooper() — the standard HandlerThread
// teardown (AoT log 44, Unity AndroidVideoMedia): the handler callback for the
// "stop" message runs `msg.getTarget().getLooper().quit()`, and the caller then
// Thread.join()s the HandlerThread. With getTarget() simulated as null the quit
// never happened, join() blocked the guest main thread forever and the game
// stopped submitting frames (compositor "stale in a row", audio all zeros).
static klj_val klj_Message_getTarget(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_message *m = klj_as_message(self);
    return (klj_val){.l = m ? m->target : NULL};
}
// The Looper the Handler was built with; a Handler made without one is bound to
// the calling thread's looper, which in this shim is the main one.
static klj_val klj_Handler_getLooper(void *env, void *self, const klj_val *a, int n) {
    klj_handler *h = klj_as_handler(self);
    if (h && h->looper) return (klj_val){.l = h->looper};
    return klj_Looper_getMainLooper(env, self, a, n);
}

// ---- android.os.HandlerThread ----
// The thread is not started here — Android requires an explicit start() — so the
// looper exists but nothing delivers on it until then.
// <init>(String name) and <init>(String name, int priority). The name is kept:
// getName() reads it back and the host thread is labelled with it.
static klj_val klj_HandlerThread_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_looper *lp = calloc(1, sizeof *lp);
    if (lp) {
        pthread_mutex_init(&lp->lock, NULL);
        pthread_cond_init(&lp->wake, NULL);
        klj_jthread_init(&lp->th, n > 0 ? klj_str(a[0].l) : NULL);
        if (n > 1) lp->th.priority = (int)(int32_t)a[1].j;
    }
    klj_as_object(obj)->data = lp;
    return (klj_val){.l = obj};
}

// getLooper() blocks on Android until the thread is running; ours is ready as
// soon as start() has spawned it, and callers only use it to build a Handler.
static klj_val klj_HandlerThread_getLooper(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    void *looper = kl_jni_new_object("android/os/Looper");
    klj_as_object(looper)->data = o ? o->data : NULL;    // shares the klj_looper
    return (klj_val){.l = looper};
}

 klj_val klj_HandlerThread_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_looper *lp = o ? o->data : NULL;
    if (!lp) {
        KLJ_LOG("HandlerThread.start() with no looper — nothing to run");
        return (klj_val){.j = 0};
    }
    if (!lp->started) {
        lp->started = lp->running = 1;
        pthread_mutex_lock(&lp->th.mu);
        lp->th.started = 1;
        pthread_mutex_unlock(&lp->th.mu);
        if (pthread_create(&lp->thread, NULL, klj_looper_thread, lp) == 0) {
            pthread_detach(lp->thread);     // join() waits on th.done, not pthread_join
            lp->th.thread = lp->thread;
        } else {
            klj_jthread_finish(&lp->th);    // never ran: report it dead, not hung
        }
        KLJ_LOG("HandlerThread.start() — looper thread running");
    }
    return (klj_val){.j = 0};
}

// HandlerThread.quit() / quitSafely() -> boolean: false if the looper never ran
// (Android: getLooper() is null before start), true otherwise.
static klj_val klj_HandlerThread_quit_common(void *self, int safely) {
    klj_object *o  = klj_as_object(self);
    klj_looper *lp = o ? o->data : NULL;
    if (!lp || !lp->started) return (klj_val){.j = 0};
    KLJ_LOG("HandlerThread.%s() \"%s\"", safely ? "quitSafely" : "quit",
            lp->th.name[0] ? lp->th.name : "?");
    klj_looper_stop(lp, safely);
    return (klj_val){.j = 1};
}
static klj_val klj_HandlerThread_quit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return klj_HandlerThread_quit_common(self, 0);
}
static klj_val klj_HandlerThread_quitSafely(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return klj_HandlerThread_quit_common(self, 1);
}

// ---- android.view.Choreographer ----
//
// Android's vsync callback, and the engine's frame clock: the guest posts a
// FrameCallback and gets doFrame(frameTimeNanos) once per display refresh. Beat
// Saber reaches it through the Android Game SDK, whose ChoreographerCallback
// registered the native nOnChoreographer(long, long).
//
// getInstance() is per-thread on Android. One instance is enough here because
// nothing distinguishes them: the callback list is what matters, and it is driven
// from the host's frame pump rather than by a real vsync source.
static klj_val klj_Choreographer_getInstance(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *instance;
    return klj_singleton("android/view/Choreographer", &instance);
}

// A posted frame callback is *one-shot* on Android: it fires at the next frame and
// is then forgotten, and a caller that wants every frame re-posts from inside
// doFrame. Keeping that exactly right matters — treating it as persistent would
// call it twice per frame once the guest re-posts, and the engine derives its
// delta time from the gap between calls.
 void *g_frame_callback;

// Forward — kl_jni_tick_choreographer() is defined later, beside the message
// delivery machinery; the frame-clock thread needs this and no other hook.
void kl_jni_tick_choreographer(void);

// ---- the frame clock -----------------------------------------------
//
// On Android the Choreographer's doFrame fires CONTINUOUSLY, once per display
// refresh, from the system — wholly independent of the render thread. Unity
// 1.40 (via the Android Game SDK's Swappy) waits on a refresh COUNTER that
// each doFrame advances by one, and the pump calling nativeRender directly
// blocked forever because it could deliver a doFrame only BEFORE the call and
// could not deliver more while inside it. The fixes are exactly as device
// reality is structured: the frame clock is a free-running source of its own,
// not a side effect of the render pump. Its cadence is read from the same
// display-frequency seam the compositor pushes (kl_ovrp_display_frequency),
// so it moves with the stated device — 72 Hz Quest-2 fiction on the host, the
// real drawable rate once a visionOS frontend pushes it.
//
// The thread calls guest code (JNIBridge.invoke -> doFrame -> the engine's own
// nOnChoreographer) while the main thread may be inside nativeRender. That is
// exactly Android: the vsync thread and the render thread are different
// threads, and the engine's own bookkeeping around these counters is locked
// (the wait side and the counter read hold the same mutex).
static int         g_frame_clock_running;
static void *klj_frame_clock_main(void *arg) {
    (void)arg;
    kl_jni_env();                     // per-thread env + kl_thread_init
    while (g_frame_clock_running) {
        double hz = kl_ovrp_display_frequency();
        if (!(hz >= 30.0 && hz <= 240.0)) hz = 72.0;
        struct timespec d = { 0, (long)(1e9 / hz) };
        nanosleep(&d, NULL);
        if (g_frame_callback)
            kl_jni_tick_choreographer();
    }
    return NULL;
}
static void klj_frame_clock_start(void) {
    static int started;
    if (started) return;
    started = 1;
    g_frame_clock_running = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, klj_frame_clock_main, NULL) == 0)
        pthread_detach(th);
}

static klj_val klj_Choreographer_postFrameCallback(void *env, void *self,
                                                   const klj_val *a, int n) {
    (void)env; (void)self;
    g_frame_callback = n > 0 ? a[0].l : NULL;
    // The moment a callback exists the frame clock must be LIVE on its own
    // thread — the render pump may block inside nativeRender waiting on the
    // refresh counter at any moment, and only an independent source can keep
    // advancing it then. See the frame-clock block above.
    klj_frame_clock_start();
    if (g_frame_callback) {
        static int announced;
        if (!announced) {
            announced = 1;
            klj_object *o = klj_as_object(g_frame_callback);
            KLJ_LOG("Choreographer.postFrameCallback(%s) — the frame clock now runs "
                    "on its own host thread at the seam's display frequency",
                    o ? o->cls : "(untagged)");
        }
    }
    return (klj_val){.j = 0};
}

// kl_jni_tick_choreographer() lives further down, next to the message delivery —
// both call into guest proxies, and that machinery is defined there.

// ---- android.os.Process ----
// setThreadPriority(tid, priority) is an Android nice value applied to another
// thread by tid. Darwin has no by-nice equivalent — scheduling is set through
// pthread QoS classes — but the ONE case that matters we CAN honour: a thread
// flagged THREAD_PRIORITY_AUDIO (-16) or _URGENT_AUDIO (-19) is an audio mixer
// thread with a realtime deadline, and on visionOS a default-QoS guest thread is
// scheduled BEHIND the compositor. Under shader-compile / render load that mixer
// starves and its output arrives in gaps — the stutter olar exhibits even though
// our CoreAudio output path never underruns (the gaps are in the content the
// guest produced). So for an audio priority we route the tid through the same
// QoS-override path XR-critical threads use (kl_pthread_boost_qos: self door for
// the calling thread, cross-thread override via the live registry otherwise).
// The tid the guest passes is our pthread_threadid_np — gettid / Process.myTid
// both return it — so the registry lookup matches. Below-normal / non-audio
// priorities stay recorded-not-applied: boosting the game or render thread here
// would starve the very compositor this is careful not to.
#define KLJ_THREAD_PRIORITY_AUDIO (-16)   // android.os.Process.THREAD_PRIORITY_AUDIO
static klj_val klj_Process_setThreadPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n > 1) {
        int tid = (int)a[0].j, prio = (int)a[1].j;
        if (prio <= KLJ_THREAD_PRIORITY_AUDIO) {
            KLJ_LOG("Process.setThreadPriority(tid=%d, %d) — audio thread, "
                    "raising to USER_INTERACTIVE QoS", tid, prio);
            kl_pthread_boost_qos((uint32_t)tid);
        } else {
            KLJ_LOG("Process.setThreadPriority(tid=%d, %d) — not applied on Darwin",
                    tid, prio);
        }
    } else if (n > 0) {
        int prio = (int)a[0].j;
        if (prio <= KLJ_THREAD_PRIORITY_AUDIO) {
            uint64_t self_tid = 0; pthread_threadid_np(NULL, &self_tid);
            KLJ_LOG("Process.setThreadPriority(%d) — audio, raising self to "
                    "USER_INTERACTIVE QoS", prio);
            kl_pthread_boost_qos(self_tid);
        } else {
            KLJ_LOG("Process.setThreadPriority(%d) — not applied on Darwin", prio);
        }
    }
    return (klj_val){0};
}
// SDL's own wrapper for the same operation, and it lands in the same place:
// it is Process.setThreadPriority(THREAD_PRIORITY_AUDIO or _URGENT_AUDIO) on
// the CALLING thread, which Darwin has no by-tid equivalent for. Recorded, not
// applied — see klj_Process_setThreadPriority above for why.
//
// Worth noting what this does NOT cost us: SDL raises the priority of a thread
// that feeds the audio device, and our audio path does not use it. kl_audio.c's
// CoreAudio render callback runs on the OS's own realtime thread and the device
// provides the clock, so the guest's feeder is a producer into a ring rather
// than something with a deadline.
static klj_val klj_SDLAM_audioSetThreadPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    if (n > 1) KLJ_LOG("SDLAudioManager.audioSetThreadPriority(recording=%d, device=%d) "
                       "— not applied on Darwin", (int)a[0].j, (int)a[1].j);
    return (klj_val){0};
}

// SDLAudioManager.unregisterAudioDeviceCallback() — drop the AudioDeviceCallback
// SDL registered so it can hear devices appear and disappear.
//
// A no-op, and the smali is what says so: the body does nothing but hand the
// callback back to AudioManager.unregisterAudioDeviceCallback and return. There
// is nothing to drop — `AudioManager.getDevices()` is answered EMPTY here (host
// devices are deliberately not enumerated), so the set SDL was watching never
// had a member and never changed. Same reasoning, and the same shape, as
// DisplayManager.registerDisplayListener: the thing being subscribed to cannot
// vary, so nothing is owed a callback and nothing is lost by forgetting one.
//
// Reached on TEARDOWN, which is why it took until a run got far enough to shut
// down cleanly to reach it: the app puts up its own "no streaming host" message
// box and then exits.
static klj_val klj_SDLAM_unregisterAudioDeviceCallback(void *env, void *self,
                                                       const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("SDLAudioManager.unregisterAudioDeviceCallback() — no devices were "
            "enumerated, so nothing was subscribed");
    return (klj_val){0};
}

static klj_val klj_Process_myTid(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (klj_val){.j = (uint32_t)tid};
}

// ---- java.lang.Thread ----------------------------------------------------
//
// AoT log 42: the run aborted on `java/lang/Thread.isAlive()Z` — Unity's
// AndroidVideoMedia tears its decoder down by checking (and then quitting and
// joining) the HandlerThread it started for SurfaceTexture callbacks. Thread had
// only start/currentThread/setName, so the whole lifecycle is served here, for
// every Thread-shaped object we hand out (klj_thread_of below):
//
//   * android/os/HandlerThread — a real host thread running the looper above;
//     alive from start() until its loop exits after quit()/quitSafely().
//   * java/lang/Thread made by NewObject — Thread(Runnable[, name]) gets a real
//     host thread on start() that runs the guest Runnable's run() through the
//     JNIBridge proxy (a guest cannot subclass Thread without dex, so a Runnable
//     is the only body it can have); alive until run() returns.
//   * Thread.currentThread() — the calling host thread, one object per thread,
//     pinned (it is cached across frames), always alive.
//
// These used to live in kl_jni_net.c (start/currentThread/setName); they moved
// here so the one struct that knows a thread's state answers every question
// about it.

static uint64_t klj_thread_self_tid(void) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return tid;
}

static klj_jthread *klj_thread_of(void *obj) {
    klj_object *o = klj_as_object(obj);
    if (!o) return NULL;
    if (strcmp(o->cls, "android/os/HandlerThread") == 0) {
        klj_looper *lp = o->data;
        return lp ? &lp->th : NULL;
    }
    if (strcmp(o->cls, "java/lang/Thread") == 0) {
        // A Thread object some other path made without a payload (AllocObject):
        // give it one, unstarted, so the questions below have an answer.
        if (!o->data) {
            klj_jthread *t = calloc(1, sizeof *t);
            if (!t) return NULL;
            klj_jthread_init(t, NULL);
            o->data = t;
        }
        return o->data;
    }
    return NULL;
}

static int klj_is_handler_thread(void *obj) {
    klj_object *o = klj_as_object(obj);
    return o && strcmp(o->cls, "android/os/HandlerThread") == 0;
}

// Thread() / Thread(Runnable) / Thread(Runnable, String) / Thread(String) /
// Thread(ThreadGroup, Runnable, String, long). The Runnable is pinned while the
// Thread holds it: the guest made it in a local frame that will pop long before
// start() runs it on another thread.
static klj_val klj_thread_new(void *clazz, void *runnable, const char *name) {
    (void)clazz;   // only java/lang/Thread itself: a guest cannot subclass it without dex
    klj_jthread *t = calloc(1, sizeof *t);
    if (!t) return (klj_val){.l = NULL};
    klj_jthread_init(t, name);
    if (runnable) { t->runnable = runnable; kl_jni_pin_object(runnable); }
    // Payload deliberately has no destructor: the object can be retired while
    // its thread still runs (the guest dropped its last ref after start()), and
    // the body reads `t` until it finishes. A few dozen bytes per Thread.
    void *obj = klj_new_object_data("java/lang/Thread", t);
    KLJ_LOG("new Thread(%s%s%s)", runnable ? "Runnable" : "",
            runnable && name ? ", " : "", name ? name : "");
    return (klj_val){.l = obj};
}
static klj_val klj_Thread_init0(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return klj_thread_new(clazz, NULL, NULL);
}
static klj_val klj_Thread_init_r(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    return klj_thread_new(clazz, n > 0 ? a[0].l : NULL, NULL);
}
static klj_val klj_Thread_init_rs(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    return klj_thread_new(clazz, n > 0 ? a[0].l : NULL, n > 1 ? klj_str(a[1].l) : NULL);
}
static klj_val klj_Thread_init_s(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    return klj_thread_new(clazz, NULL, n > 0 ? klj_str(a[0].l) : NULL);
}
static klj_val klj_Thread_init_grsj(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;   // (ThreadGroup group, Runnable target, String name, long stackSize)
    return klj_thread_new(clazz, n > 1 ? a[1].l : NULL, n > 2 ? klj_str(a[2].l) : NULL);
}

// The body of a started java/lang/Thread: run the guest Runnable on this host
// thread. kl_jni_env() gives the thread its JNIEnv and kl_thread_init (guest code
// runs here, so the stack-protector TSD slot must be set before the first call).
static void *klj_thread_main(void *arg) {
    klj_jthread *t = arg;
    kl_jni_env();
    pthread_mutex_lock(&t->mu);
    t->tid = klj_thread_self_tid();
    pthread_mutex_unlock(&t->mu);
    if (t->name[0]) pthread_setname_np(t->name);
    if (t->runnable)
        klj_proxy_invoke(t->runnable, "java/lang/Runnable", "run", "()V", NULL);
    void *runnable = t->runnable, *self = t->self_obj;
    klj_jthread_finish(t);
    KLJ_LOG("Thread \"%s\" run() returned", t->name[0] ? t->name : "?");
    if (runnable) kl_jni_unpin_object(runnable);
    if (self) kl_jni_unpin_object(self);
    return NULL;
}

// Thread.start(). A HandlerThread starts its looper (the guest resolves start()
// on java/lang/Thread and calls it on the HandlerThread). A Thread(Runnable) gets
// a host thread that runs the Runnable. A Thread with no Runnable has an empty
// run() — it starts and finishes at once, which is what Java does.
static klj_val klj_Thread_start(void *env, void *self, const klj_val *a, int n) {
    if (klj_is_handler_thread(self))
        return klj_HandlerThread_start(env, self, a, n);
    klj_jthread *t = klj_thread_of(self);
    if (!t) {
        klj_object *o = klj_as_object(self);
        KLJ_LOG("Thread.start() on %s — not a thread we model; NOT started",
                o ? o->cls : "(not one of our objects)");
        return (klj_val){.j = 0};
    }
    pthread_mutex_lock(&t->mu);
    if (t->started || t->is_current) {
        pthread_mutex_unlock(&t->mu);
        KLJ_LOG("Thread.start() on a thread already started (IllegalThreadStateException "
                "on Android) — ignored");
        return (klj_val){.j = 0};
    }
    t->started = 1;
    pthread_mutex_unlock(&t->mu);
    if (!t->runnable) {
        KLJ_LOG("Thread.start() with no Runnable — run() is empty, finished at once");
        klj_jthread_finish(t);
        return (klj_val){.j = 0};
    }
    t->self_obj = self;
    kl_jni_pin_object(self);            // alive while its body runs
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);   // Android's Java default
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->thread, &attr, klj_thread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        KLJ_LOG("Thread.start(): pthread_create failed (%d) — reporting it finished", rc);
        kl_jni_unpin_object(self);
        t->self_obj = NULL;
        klj_jthread_finish(t);
    } else {
        KLJ_LOG("Thread.start() \"%s\" — host thread running the guest Runnable",
                t->name[0] ? t->name : "?");
    }
    return (klj_val){.j = 0};
}

// Thread.run() called directly: Java runs the Runnable on the CALLER's thread.
static klj_val klj_Thread_run(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    if (klj_is_handler_thread(self)) {
        KLJ_LOG("HandlerThread.run() called directly — the looper runs on its own "
                "thread after start(); ignored");
        return (klj_val){.j = 0};
    }
    klj_jthread *t = klj_thread_of(self);
    if (t && t->runnable)
        klj_proxy_invoke(t->runnable, "java/lang/Runnable", "run", "()V", NULL);
    return (klj_val){.j = 0};
}

// Thread.currentThread() — one object per host thread, pinned (the cache below
// outlives every local frame; unpinned it was retired at the first pop and the
// cached pointer went stale), carrying this thread's identity.
static klj_val klj_Thread_currentThread(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static _Thread_local void *me;
    if (!me || !klj_as_object(me)) {
        klj_jthread *t = calloc(1, sizeof *t);
        if (!t) return (klj_val){.l = NULL};
        char nm[64] = {0};
        pthread_getname_np(pthread_self(), nm, sizeof nm);
        klj_jthread_init(t, nm[0] ? nm : NULL);
        t->is_current = t->started = 1;
        t->thread = pthread_self();
        t->tid = klj_thread_self_tid();
        me = klj_new_object_data("java/lang/Thread", t);
        kl_jni_pin_object(me);
    }
    return (klj_val){.l = me};
}

// isAlive(): started and its body has not returned. The current thread is
// alive by definition.
static klj_val klj_Thread_isAlive(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    if (!t) return (klj_val){.j = 0};
    pthread_mutex_lock(&t->mu);
    int alive = t->is_current || (t->started && !t->finished);
    pthread_mutex_unlock(&t->mu);
    static int said;
    if (said++ < 8)
        KLJ_LOG("Thread.isAlive() \"%s\" -> %s", t->name[0] ? t->name : "?",
                alive ? "true" : "false");
    return (klj_val){.j = (uint64_t)alive};
}

// join() / join(millis) / join(millis, nanos). Returns when the body has
// returned, or the timeout passed (0 = forever, as in Java). Joining yourself
// would block forever on Android; here it returns at once and says so. A join
// on a HandlerThread nobody quit really would never return — that is Android's
// behaviour too, so it is kept, with a line every 5 s naming the wait.
static void klj_thread_join(klj_jthread *t, int64_t millis) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    if (!t->started || t->is_current ||
        (t->thread && pthread_equal(t->thread, pthread_self()))) {
        if (t->started && !t->finished)
            KLJ_LOG("Thread.join() on the calling thread itself — returning");
        pthread_mutex_unlock(&t->mu);
        return;
    }
    uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    unsigned waits = 0;
    while (!t->finished) {
        int64_t slice_ms = 5000;
        if (millis > 0) {
            int64_t left = millis - (int64_t)((clock_gettime_nsec_np(CLOCK_MONOTONIC) - start) / 1000000ull);
            if (left <= 0) break;
            if (left < slice_ms) slice_ms = left;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(slice_ms / 1000);
        ts.tv_nsec += (long)(slice_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&t->done, &t->mu, &ts);
        if (!t->finished && millis <= 0 && ++waits <= 6)
            KLJ_LOG("Thread.join() \"%s\" still waiting (%u x 5 s) — the thread has not "
                    "finished (a HandlerThread needs quit() first)",
                    t->name[0] ? t->name : "?", waits);
    }
    pthread_mutex_unlock(&t->mu);
}
static klj_val klj_Thread_join(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_thread_join(klj_thread_of(self), n > 0 ? (int64_t)a[0].j : 0);
    return (klj_val){.j = 0};
}

static klj_val klj_Thread_interrupt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    if (t) { pthread_mutex_lock(&t->mu); t->interrupted = 1; pthread_mutex_unlock(&t->mu); }
    return (klj_val){.j = 0};
}
static klj_val klj_Thread_isInterrupted(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    return (klj_val){.j = (uint64_t)(t ? t->interrupted : 0)};
}
// static Thread.interrupted(): test-and-clear the CALLING thread's flag.
static klj_val klj_Thread_interrupted(void *env, void *self, const klj_val *a, int n) {
    klj_jthread *t = klj_thread_of(klj_Thread_currentThread(env, self, a, n).l);
    if (!t) return (klj_val){.j = 0};
    pthread_mutex_lock(&t->mu);
    int was = t->interrupted; t->interrupted = 0;
    pthread_mutex_unlock(&t->mu);
    return (klj_val){.j = (uint64_t)was};
}

static klj_val klj_Thread_getName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    char buf[80];
    if (t && t->name[0]) snprintf(buf, sizeof buf, "%s", t->name);
    else snprintf(buf, sizeof buf, "Thread-%llu", (unsigned long long)(t ? t->tid : 0));
    return (klj_val){.l = kl_jni_new_string(buf)};
}

// setName is APPLIED to the host thread when the object names the calling
// thread (UE4 labels its own pthreads this way, which is what makes `sample`
// and fault reports readable); for any other thread it is recorded and applied
// when that thread starts. Darwin caps names at 64 bytes, so it is truncated.
static klj_val klj_Thread_setName(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *name = (n >= 1) ? klj_str(a[0].l) : NULL;
    if (!name || !*name) return (klj_val){.j = 0};
    klj_jthread *t = klj_thread_of(self);
    if (t) {
        pthread_mutex_lock(&t->mu);
        snprintf(t->name, sizeof t->name, "%s", name);
        pthread_mutex_unlock(&t->mu);
    }
    if (!t || t->is_current) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", name);
        pthread_setname_np(buf);
    }
    return (klj_val){.j = 0};
}

static klj_val klj_Thread_getId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    // Unstarted threads have no host tid yet; Java ids are unique and stable,
    // so the object's own address stands in.
    uint64_t id = t ? (t->tid ? t->tid : (uint64_t)(uintptr_t)t) : 1;
    return (klj_val){.j = id};
}
// HandlerThread.getThreadId() -> int: the tid, or -1 before the looper runs.
static klj_val klj_HandlerThread_getThreadId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    int32_t id = (t && t->tid) ? (int32_t)t->tid : -1;
    return (klj_val){.j = (uint64_t)(int64_t)id};
}

// Priority and daemon status are recorded and read back; Darwin schedules by
// QoS, and an Android nice level has no faithful mapping (see Process below).
static klj_val klj_Thread_setPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_jthread *t = klj_thread_of(self);
    if (t && n > 0) t->priority = (int)(int32_t)a[0].j;
    return (klj_val){.j = 0};
}
static klj_val klj_Thread_getPriority(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    return (klj_val){.j = (uint64_t)(int64_t)(t ? t->priority : 5)};
}
static klj_val klj_Thread_setDaemon(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_jthread *t = klj_thread_of(self);
    if (t && n > 0) t->daemon = a[0].j ? 1 : 0;
    return (klj_val){.j = 0};
}
static klj_val klj_Thread_isDaemon(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_jthread *t = klj_thread_of(self);
    return (klj_val){.j = (uint64_t)(t ? t->daemon : 0)};
}

// static sleep(millis[, nanos]) and yield().
static klj_val klj_Thread_sleep(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int64_t ms = n > 0 ? (int64_t)a[0].j : 0;
    int64_t ns = n > 1 ? (int64_t)(int32_t)a[1].j : 0;
    if (ms < 0) ms = 0;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L + (long)ns };
    while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    nanosleep(&ts, NULL);
    return (klj_val){.j = 0};
}
static klj_val klj_Thread_yield(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    sched_yield();
    return (klj_val){.j = 0};
}

// getStackTrace(): an empty StackTraceElement[] — there is no Java stack here,
// and an empty trace is a legal answer callers iterate over without error.
static klj_val klj_Thread_getStackTrace(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L', "java/lang/StackTraceElement", 0)};
}

// setUncaughtExceptionHandler / setDefaultUncaughtExceptionHandler: no Java
// code runs here, so nothing can throw uncaught through it. Recorded-as-nothing.
static klj_val klj_Thread_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

const klj_binding klj_bind_looper[] = {
    {"android/app/Activity",   "runOnUiThread", "(Ljava/lang/Runnable;)V", klj_Activity_runOnUiThread},

    {"org/libsdl/app/SDLAudioManager", "unregisterAudioDeviceCallback", "()V",
     klj_SDLAM_unregisterAudioDeviceCallback},
    {"org/libsdl/app/SDLAudioManager", "audioSetThreadPriority", "(ZI)V",
     klj_SDLAM_audioSetThreadPriority},
    {"android/os/Process", "setThreadPriority", "(II)V", klj_Process_setThreadPriority},
    {"android/os/Process", "setThreadPriority", "(I)V",  klj_Process_setThreadPriority},
    {"android/os/Process", "myTid",             "()I",   klj_Process_myTid},

    {"android/os/Looper",  "getMainLooper", "()Landroid/os/Looper;",  klj_Looper_getMainLooper},
    {"android/os/Looper",  "myLooper",      "()Landroid/os/Looper;",  klj_Looper_myLooper},
    {"android/os/Looper",  "prepare",       "()V",                    klj_Looper_prepare},
    {"android/os/Looper",  "getQueue",      "()Landroid/os/MessageQueue;", klj_Looper_getQueue},
    {"android/os/Looper",  "quit",          "()V",                    klj_Looper_quit},
    {"android/os/MessageQueue", "next",     "()Landroid/os/Message;", klj_MessageQueue_next},
    // A HandlerThread is a thread with a Looper on it. We have one queue and one
    // drain point (kl_jni_drain_ui_tasks) for the main looper — but a
    // HandlerThread gets a real thread of its own, because the guest blocks
    // waiting on it. See the looper section above.
    {"android/os/Handler", "obtainMessage", "(I)Landroid/os/Message;", klj_Handler_obtainMessage},
    {"android/os/Message", "sendToTarget", "()V", klj_Message_sendToTarget},
    {"android/os/Message", "getTarget", "()Landroid/os/Handler;", klj_Message_getTarget},
    {"android/os/Handler", "getLooper", "()Landroid/os/Looper;", klj_Handler_getLooper},
    {"android/view/Choreographer", "getInstance", "()Landroid/view/Choreographer;", klj_Choreographer_getInstance},
    {"android/view/Choreographer", "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", klj_Choreographer_postFrameCallback},
    {"android/os/HandlerThread", "<init>", "(Ljava/lang/String;)V", klj_HandlerThread_init},
    {"android/os/HandlerThread", "start", "()V", klj_HandlerThread_start},
    {"android/os/HandlerThread", "getLooper", "()Landroid/os/Looper;", klj_HandlerThread_getLooper},
    {"android/os/HandlerThread", "<init>", "(Ljava/lang/String;I)V", klj_HandlerThread_init},
    {"android/os/HandlerThread", "quit",        "()Z", klj_HandlerThread_quit},
    {"android/os/HandlerThread", "quitSafely",  "()Z", klj_HandlerThread_quitSafely},
    {"android/os/HandlerThread", "getThreadId", "()I", klj_HandlerThread_getThreadId},
    {"android/os/Looper",        "quitSafely",  "()V", klj_Looper_quitSafely},

    // java.lang.Thread — the whole lifecycle (see the Thread section above).
    // Bound on java/lang/Thread; HandlerThread reaches these through the
    // HandlerThread -> Thread edge in kl_jni.c's g_supers as well as directly
    // (the guest resolves them on java/lang/Thread and passes the HandlerThread).
    {"java/lang/Thread", "<init>", "()V",                                       klj_Thread_init0},
    {"java/lang/Thread", "<init>", "(Ljava/lang/Runnable;)V",                   klj_Thread_init_r},
    {"java/lang/Thread", "<init>", "(Ljava/lang/Runnable;Ljava/lang/String;)V", klj_Thread_init_rs},
    {"java/lang/Thread", "<init>", "(Ljava/lang/String;)V",                     klj_Thread_init_s},
    {"java/lang/Thread", "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V",
     klj_Thread_init_grsj},
    {"java/lang/Thread", "start",          "()V",                  klj_Thread_start},
    {"java/lang/Thread", "run",            "()V",                  klj_Thread_run},
    {"java/lang/Thread", "currentThread",  "()Ljava/lang/Thread;", klj_Thread_currentThread},
    {"java/lang/Thread", "isAlive",        "()Z",                  klj_Thread_isAlive},
    {"java/lang/Thread", "join",           "()V",                  klj_Thread_join},
    {"java/lang/Thread", "join",           "(J)V",                 klj_Thread_join},
    {"java/lang/Thread", "join",           "(JI)V",                klj_Thread_join},
    {"java/lang/Thread", "interrupt",      "()V",                  klj_Thread_interrupt},
    {"java/lang/Thread", "isInterrupted",  "()Z",                  klj_Thread_isInterrupted},
    {"java/lang/Thread", "interrupted",    "()Z",                  klj_Thread_interrupted},
    {"java/lang/Thread", "getName",        "()Ljava/lang/String;", klj_Thread_getName},
    {"java/lang/Thread", "setName",        "(Ljava/lang/String;)V", klj_Thread_setName},
    {"java/lang/Thread", "getId",          "()J",                  klj_Thread_getId},
    {"java/lang/Thread", "setPriority",    "(I)V",                 klj_Thread_setPriority},
    {"java/lang/Thread", "getPriority",    "()I",                  klj_Thread_getPriority},
    {"java/lang/Thread", "setDaemon",      "(Z)V",                 klj_Thread_setDaemon},
    {"java/lang/Thread", "isDaemon",       "()Z",                  klj_Thread_isDaemon},
    {"java/lang/Thread", "sleep",          "(J)V",                 klj_Thread_sleep},
    {"java/lang/Thread", "sleep",          "(JI)V",                klj_Thread_sleep},
    {"java/lang/Thread", "yield",          "()V",                  klj_Thread_yield},
    {"java/lang/Thread", "getStackTrace",  "()[Ljava/lang/StackTraceElement;", klj_Thread_getStackTrace},
    {"java/lang/Thread", "setUncaughtExceptionHandler",
     "(Ljava/lang/Thread$UncaughtExceptionHandler;)V", klj_Thread_void},
    {"java/lang/Thread", "setDefaultUncaughtExceptionHandler",
     "(Ljava/lang/Thread$UncaughtExceptionHandler;)V", klj_Thread_void},
    {"android/os/Handler", "<init>", "()V",                        klj_Handler_init},
    {"android/os/Handler", "<init>", "(Landroid/os/Looper;)V",     klj_Handler_init},
    // The Callback form. The callback handles Messages sent through this Handler,
    // and nothing here sends Messages — the queue only ever carries Runnables from
    // post/postDelayed — so recording the Looper and dropping the callback is the
    // same single-queue simplification, not a new one. If a Message ever is sent,
    // sendMessage is unimplemented and will say so by name.
    {"android/os/Handler", "<init>", "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", klj_Handler_init},
    {"android/os/Handler", "post",        "(Ljava/lang/Runnable;)Z",  klj_Handler_post},
    {"android/os/Handler", "postDelayed", "(Ljava/lang/Runnable;J)Z", klj_Handler_postDelayed},
    {0}
};
