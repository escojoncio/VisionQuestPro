#!/usr/bin/env python3
"""Small, idempotent source patches to VK-GL-CTS for the Android *executable* build that
VisionQuestPro runs in-process (no ANativeActivity behind it).

1. deqp's module executables must export their entry (vq_exe_main, wrapping main's body) (Klepton's executable door calls it by name);
   CMAKE_EXE_LINKER_FLAGS does not survive deqp's own toolchain setup, so the option goes on
   the target.
2. Platform::describePlatform dereferences the activity's JavaVM; in executable mode the activity
   is NULL. Describe the platform without Java instead.
Usage: patch_deqp.py <VK-GL-CTS checkout>
"""
import pathlib, sys

root = pathlib.Path(sys.argv[1])

def patch(rel, old, new, count=1):
    p = root / rel
    s = p.read_text()
    if new in s:
        print(f"{rel}: already patched")
        return
    if s.count(old) != count:
        sys.exit(f"{rel}: anchor found {s.count(old)} times, expected {count}")
    p.write_text(s.replace(old, new))
    print(f"{rel}: patched")

patch("CMakeLists.txt",
      "\t\tadd_executable(${MODULE_NAME} ${PROJECT_SOURCE_DIR}/framework/platform/tcuMain.cpp ${ENTRY})\n",
      "\t\tadd_executable(${MODULE_NAME} ${PROJECT_SOURCE_DIR}/framework/platform/tcuMain.cpp ${ENTRY})\n"
      "\t\tif (DE_OS_IS_ANDROID)\n"
      "\t\t\ttarget_link_options(${MODULE_NAME} PRIVATE \"-Wl,--export-dynamic-symbol=vq_exe_main\" \"-Wl,--export-dynamic-symbol=main\")\n"
      "\t\tendif ()\n", count=2)

# A C entry with default visibility that the dynamic table always carries (an executable's own
# main() is not exported by default, whatever the visibility preset of the build).
patch("framework/platform/tcuMain.cpp",
      "int main(int argc, char **argv)\n",
      "extern \"C\" __attribute__((visibility(\"default\"), used)) int vq_exe_main(int argc, char **argv);\n"
      "int main(int argc, char **argv)\n{\n    return vq_exe_main(argc, argv);\n}\n\n"
      "extern \"C\" __attribute__((visibility(\"default\"), used)) int vq_exe_main(int argc, char **argv)\n")

# createPlatform() hands Platform a reference to a NativeActivity on its own stack: every later
# use of m_activity (describePlatform in dEQP-VK.info.platform) read a dead stack slot and
# crashed (signal 11, log 60). Static storage keeps the (NULL) activity alive.
patch("framework/platform/android/tcuAndroidPlatform.cpp",
      "    tcu::Android::NativeActivity activity(NULL);\n    return new tcu::Android::Platform(activity);\n",
      "    static tcu::Android::NativeActivity activity(NULL);\n    return new tcu::Android::Platform(activity);\n")

patch("framework/platform/android/tcuAndroidPlatform.cpp",
      "void Platform::describePlatform(std::ostream &dst) const\n{\n",
      "void Platform::describePlatform(std::ostream &dst) const\n{\n"
      "    if (!m_activity.getNativeActivity())\n"
      "    {\n"
      "        dst << \"Android executable (VisionQuestPro guest, no ANativeActivity)\\n\";\n"
      "        return;\n"
      "    }\n")
