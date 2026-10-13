#!/usr/bin/env python3
"""Small, idempotent source patches to VK-GL-CTS for the Android *executable* build that
VisionQuestPro runs in-process (no ANativeActivity behind it).

1. Platform::describePlatform dereferences the activity's JavaVM; in executable mode the activity
   is NULL. Describe the platform without Java instead.
Usage: patch_deqp.py <VK-GL-CTS checkout>
"""
import pathlib, sys

root = pathlib.Path(sys.argv[1])

def patch(rel, old, new):
    p = root / rel
    s = p.read_text()
    if new in s:
        print(f"{rel}: already patched")
        return
    if s.count(old) != 1:
        sys.exit(f"{rel}: anchor not found exactly once")
    p.write_text(s.replace(old, new))
    print(f"{rel}: patched")

patch("framework/platform/android/tcuAndroidPlatform.cpp",
      "void Platform::describePlatform(std::ostream &dst) const\n{\n",
      "void Platform::describePlatform(std::ostream &dst) const\n{\n"
      "    if (!m_activity.getNativeActivity())\n"
      "    {\n"
      "        dst << \"Android executable (VisionQuestPro guest, no ANativeActivity)\\n\";\n"
      "        return;\n"
      "    }\n")
