// Host test of vq_config.c's vq_cache_complete: the shader cache is complete when the engine reports
// complete, or when its only failing criterion is the unused-module share (reason 5: it never holds when a
// game has many unused recipes, e.g. Maestro 78 used of 225). Any other reason (no pipelines, new keys, no
// frames, compiling) keeps it incomplete.
#include <stdio.h>
#include "vq_config.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static struct vq_cache_state st(uint32_t complete, uint32_t reason) {
    struct vq_cache_state s = {1, 200, 225, 225, 78, 30000, 900, complete, reason};
    return s;
}

int main(void) {
    struct vq_cache_state s;
    s = st(1, 0); CHECK(vq_cache_complete(&s) == 1, "engine complete -> complete");
    s = st(0, 5); CHECK(vq_cache_complete(&s) == 1, "only unused modules failing -> complete");
    s = st(0, 1); CHECK(vq_cache_complete(&s) == 0, "no pipelines -> incomplete");
    s = st(0, 2); CHECK(vq_cache_complete(&s) == 0, "recent new key -> incomplete");
    s = st(0, 3); CHECK(vq_cache_complete(&s) == 0, "too few frames -> incomplete");
    s = st(0, 4); CHECK(vq_cache_complete(&s) == 0, "compiling -> incomplete");
    // Defensive: the engine only reports reason 5 with pipelines (it needs pipelines_unique > 0 to get that far).
    s = st(0, 5); s.pipelines_unique = 0; CHECK(vq_cache_complete(&s) == 0, "reason 5 without pipelines -> incomplete (defensive)");
    CHECK(vq_cache_complete(NULL) == 0, "NULL -> incomplete");
    // What is stored on disk: a complete=true saved by an earlier session survives a session that added no recipes
    // (a short one that only serves keys from the cache); with new recipes it is degraded when no longer complete.
    CHECK(vq_cache_persisted_complete(1, 0, 0) == 1, "saved complete, no new recipes, now incomplete -> keep complete");
    CHECK(vq_cache_persisted_complete(1, 0, 3) == 0, "saved complete, new recipes, now incomplete -> degrade");
    CHECK(vq_cache_persisted_complete(1, 1, 3) == 1, "saved complete, new recipes, complete -> complete");
    CHECK(vq_cache_persisted_complete(0, 0, 0) == 0, "not saved complete, incomplete -> incomplete");
    CHECK(vq_cache_persisted_complete(0, 1, 0) == 1, "not saved complete, complete now -> complete");
    printf(fails ? "t_cache_complete: %d FAILED\n" : "t_cache_complete: ok\n", fails);
    return fails != 0;
}
