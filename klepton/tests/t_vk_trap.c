// klvk_missing_trap / klvk_missing_stub of kl_vulkan.c (KL-TEST vktrap block, cut out at build time).
// The stub handed to the guest for a Vulkan entry we do not provide must not claim success (that left
// Get*/Create* outputs untouched while saying they were valid), except for the entries that return a
// value that is not a VkResult, which answer 0. Each stub is called through its exact type.
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "vktrap.inc"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

typedef VkResult (VKAPI_PTR *res_fn)(void);
typedef uint64_t (VKAPI_PTR *zero_fn)(void);

int main(void) {
    CHECK(klvk_missing_trap() == VK_ERROR_EXTENSION_NOT_PRESENT, "trap returns EXTENSION_NOT_PRESENT");
    CHECK(klvk_missing_trap() != VK_SUCCESS, "trap does not claim success");
    CHECK(klvk_missing_trap_zero() == 0, "zero trap returns 0");

    static const char *const zero[] = { "vkGetAccelerationStructureDeviceAddressKHR", "vkGetBufferOpaqueCaptureAddress",
        "vkGetDeferredOperationMaxConcurrencyKHR", "vkGetImageViewHandleNVX",
        "vkGetPhysicalDeviceWaylandPresentationSupportKHR" };
    for (size_t i = 0; i < sizeof zero / sizeof zero[0]; i++) {
        PFN_vkVoidFunction f = klvk_missing_stub(zero[i]);
        CHECK(f == (PFN_vkVoidFunction)klvk_missing_trap_zero, zero[i]);
        CHECK(((zero_fn)f)() == 0, "zero stub answers 0");
    }
    static const char *const res[] = { "vkCreateDisplayModeKHR", "vkCreateValidationCacheEXT", "vkQueueBindSparse" };
    for (size_t i = 0; i < sizeof res / sizeof res[0]; i++) {
        PFN_vkVoidFunction f = klvk_missing_stub(res[i]);
        CHECK(f == (PFN_vkVoidFunction)klvk_missing_trap, res[i]);
        CHECK(((res_fn)f)() == VK_ERROR_EXTENSION_NOT_PRESENT, "result stub answers -7");
    }
    if (fails) return 1;
    printf("t_vk_trap ok\n");
    return 0;
}
