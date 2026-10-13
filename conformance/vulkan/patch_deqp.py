#!/usr/bin/env python3
"""Small, idempotent source patches to VK-GL-CTS for the Android *executable* build that
VisionQuestPro runs in-process (no ANativeActivity behind it).

1. deqp's module executables must export main() (Klepton's executable door calls it by name);
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
      "\t\t\ttarget_link_options(${MODULE_NAME} PRIVATE \"-Wl,--export-dynamic-symbol=main\")\n"
      "\t\tendif ()\n", count=2)

patch("framework/platform/android/tcuAndroidPlatform.cpp",
      "void Platform::describePlatform(std::ostream &dst) const\n{\n",
      "void Platform::describePlatform(std::ostream &dst) const\n{\n"
      "    if (!m_activity.getNativeActivity())\n"
      "    {\n"
      "        dst << \"Android executable (VisionQuestPro guest, no ANativeActivity)\\n\";\n"
      "        return;\n"
      "    }\n")
