#!/usr/bin/env python3
"""Vulkan CTS case lists for VisionQuestPro's in-headset conformance runs.

The Khronos mustpass (vk-default, ~2 million cases) is far beyond what a headset can run in a
session, and most of it covers features no Quest game touches (ray tracing, video, mesh shaders,
WSI on desktop compositors...). This picks what Quest games exercise on an Adreno 740 driver and
splits it into batches the app runs one at a time (a batch that crashes is recorded and skipped
on the next run, like the Khronos runner does on Android).

Profiles
  rapida    deterministic sample of every relevant group (~25k cases): the first, full picture.
  completa  every case of the relevant groups (~1.5M; only with VQ_CTS_FULL=1, not shipped yet).

The sample is stable across runs and builds (CRC32 of the case name), so a failure stays in the
list until it is fixed and the pass rate is comparable from one build to the next.

Usage: caselists.py <mustpass/main dir> <out dir>
Writes <out>/<profile>/NNN-<group>.txt and <out>/index.txt ("profile<TAB>file<TAB>group<TAB>count").
"""
import os, sys, zlib

# group (mustpass file stem, or directory for nested lists), stride for "rapida", and name
# prefixes (after "dEQP-VK.") that are left out of both profiles. Order = run order: the cheap,
# foundational groups first so an early failure points at the cause rather than a symptom.
GROUPS = [
    ("info",                  1,  []),
    ("api",                   3,  ["api.copy_and_blit.", "api.image_clearing.", "api.external.",
                                   "api.image_compression_control.", "api.device_group.",
                                   "api.object_management.max_concurrent.", "api.object_management.multithreaded"]),
    ("api",                 120,  None),   # second pass over api: only the heavy sub-groups, sampled
    ("memory",                4,  ["memory.external_memory_host.", "memory.device_group_allocation."]),
    ("multiview",             1,  []),
    ("binding-model",        60,  ["binding_model.mutable_descriptor.", "binding_model.descriptor_buffer.",
                                   "binding_model.descriptor_heap."]),
    ("pipeline/monolithic", 150,  []),
    ("renderpasses",         24,  ["renderpasses.dynamic_rendering."]),
    ("draw",                  6,  ["draw.dynamic_rendering."]),
    ("depth",                 6,  []),
    ("clipping",              2,  []),
    ("rasterization",         12,  []),
    ("fragment-operations",   4,  []),
    ("dynamic-state",         4,  []),
    ("texture",               12,  []),
    ("image",                40,  ["image.host_image_copy.", "image.swapchain_mutable."]),
    ("compute",              20,  ["compute.shader_object_spirv.", "compute.shader_object_binary."]),
    ("glsl",                  12,  []),
    ("ubo",                  20,  []),
    ("ssbo",                 20,  []),
    ("spirv-assembly",       60,  []),
    ("query-pool",           20,  []),
    ("synchronization",      50,  []),
    ("synchronization2",     80,  []),
    ("subgroups",            40,  []),
    ("geometry",              6,  []),
    ("tessellation",          6,  []),
    ("robustness",           120,  []),
]
# The heavy api sub-groups the second "api" entry samples (and the first one skips).
API_HEAVY = ["api.copy_and_blit.", "api.image_clearing."]

BATCH = 400           # cases per batch: short, so Stop and the time budget act within a few minutes


def read_list(path):
    out = []
    if os.path.isdir(path):
        for root, _, files in os.walk(path):
            for f in sorted(files):
                if f.endswith(".txt"):
                    out += read_list(os.path.join(root, f))
        return out
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#"):
                out.append(line)
    return out


def keep_sample(name, stride):
    return stride <= 1 or zlib.crc32(name.encode()) % stride == 0


def main():
    src, dst = sys.argv[1], sys.argv[2]
    base = os.path.join(src, "vk-default")
    index = []
    seq = 0
    totals = {"rapida": 0, "completa": 0}
    for gi, (group, stride, skip) in enumerate(GROUPS):
        path = os.path.join(base, group)
        path = path if os.path.isdir(path) else path + ".txt"
        if not os.path.exists(path):
            print(f"warning: {group} not in this mustpass", file=sys.stderr)
            continue
        names = read_list(path)
        if skip is None:          # the sampled pass over the heavy api groups
            picked = [n for n in names if any(n.startswith("dEQP-VK." + p) for p in API_HEAVY)]
        else:
            picked = [n for n in names if not any(n.startswith("dEQP-VK." + p) for p in skip)]
        label = group.replace("/", "-") + ("-heavy" if skip is None else "")
        profiles = (("rapida", stride), ("completa", 1)) if os.environ.get("VQ_CTS_FULL") == "1" \
            else (("rapida", stride),)
        for profile, s in profiles:
            cases = [n for n in picked if keep_sample(n, s)]
            if profile == "completa" and skip is None:
                cases = picked
            os.makedirs(os.path.join(dst, profile), exist_ok=True)
            for b in range(0, len(cases), BATCH):
                chunk = cases[b:b + BATCH]
                part = b // BATCH + 1
                fname = f"{seq:03d}-{label}-{part:03d}.txt"
                seq += 1
                with open(os.path.join(dst, profile, fname), "w") as fh:
                    fh.write("\n".join(chunk) + "\n")
                index.append((profile, fname, label, len(chunk)))
                totals[profile] += len(chunk)
    with open(os.path.join(dst, "index.txt"), "w") as fh:
        for row in index:
            fh.write("\t".join(str(x) for x in row) + "\n")
    print(f"rapida: {totals['rapida']} cases, completa: {totals['completa']} cases, "
          f"{len(index)} batch files")


if __name__ == "__main__":
    main()
