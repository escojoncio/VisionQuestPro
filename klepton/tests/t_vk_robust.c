// Host test of kl_vulkan.c (KL-TEST robust block): robustBufferAccess as the guest requests it
// (pEnabledFeatures / VkPhysicalDeviceFeatures2 in pNext) and its persistence as <cache>/vq.robust.
// Build: tools/ci/host-tests.sh (ASan + UBSan). Needs the Vulkan headers (-I<headers>).
#define VK_NO_PROTOTYPES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "robust.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int read_value(const char *dir) {
    char p[512], b[8] = {0};
    snprintf(p, sizeof p, "%s/vq.robust", dir);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    return n >= 2 && b[1] == '\n' && (b[0] == '0' || b[0] == '1') ? b[0] - '0' : -2;
}

int main(void) {
    VkDeviceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    CHECK(klvk_requested_robust(NULL) == 0, "null create info");
    CHECK(klvk_requested_robust(&ci) == 0, "no features, no pNext");

    VkPhysicalDeviceFeatures off = {0}, on = {0};
    on.robustBufferAccess = VK_TRUE;
    ci.pEnabledFeatures = &off;
    CHECK(klvk_requested_robust(&ci) == 0, "pEnabledFeatures off");
    ci.pEnabledFeatures = &on;
    CHECK(klvk_requested_robust(&ci) == 1, "pEnabledFeatures on");

    // Features2 in pNext (pEnabledFeatures must be NULL then), behind an unrelated struct.
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkBaseOutStructure other = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    ci.pEnabledFeatures = NULL;
    ci.pNext = &f2;
    CHECK(klvk_requested_robust(&ci) == 0, "Features2 off");
    f2.features.robustBufferAccess = VK_TRUE;
    CHECK(klvk_requested_robust(&ci) == 1, "Features2 on");
    other.pNext = (VkBaseOutStructure *)&f2;
    ci.pNext = &other;
    CHECK(klvk_requested_robust(&ci) == 1, "Features2 on after another struct");
    f2.features.robustBufferAccess = VK_FALSE;
    CHECK(klvk_requested_robust(&ci) == 0, "Features2 off after another struct");

    // Persistence.
    char tmpl[] = "/tmp/t_vk_robustXXXXXX";
    char *base = mkdtemp(tmpl);
    CHECK(base != NULL, "mkdtemp");
    if (!base) return 1;
    char dir[512];
    snprintf(dir, sizeof dir, "%s/cache", base);   // does not exist yet
    int eff = -1;
    CHECK(klvk_robust_store(NULL, 0, &eff) == -1 && eff == 0, "no dir");
    CHECK(klvk_robust_store("", 0, NULL) == -1, "empty dir");
    CHECK(klvk_robust_store(dir, 0, &eff) == 1 && eff == 0, "first write (0)");
    CHECK(read_value(dir) == 0, "stored 0");
    CHECK(klvk_robust_store(dir, 0, NULL) == 0, "same value is not rewritten");
    CHECK(klvk_robust_store(dir, 1, &eff) == 1 && eff == 1, "a request raises it to 1");
    CHECK(read_value(dir) == 1, "stored 1");
    CHECK(klvk_robust_store(dir, 0, &eff) == 0 && eff == 1, "sticky: a later device without it does not lower it");
    CHECK(read_value(dir) == 1, "still 1");
    CHECK(klvk_robust_store(dir, 5, &eff) == 0 && eff == 1, "any non-zero is 1");
    char missing[600];
    snprintf(missing, sizeof missing, "%s/no/such/parent", base);
    CHECK(klvk_robust_store(missing, 1, NULL) == -1, "missing parent fails without crashing");

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0) printf("warn: cleanup failed\n");
    if (fails) return 1;
    printf("t_vk_robust ok\n");
    return 0;
}
