#!/usr/bin/env python3
"""dEQP-GLES2 / GLES3 / GLES31 case lists for VPKEngine's own OpenGL ES (vpgles) on the headset.

Same scheme as conformance/vulkan/caselists.py: groups in run order (foundations first), a stable
CRC32 sample per group for the quick profile, batches of BATCH cases, and an index the app reads.
The modules (deqp-gles2, deqp-gles3, deqp-gles31) are built from the same VK-GL-CTS checkout as the
Vulkan CTS (modules/gles2, gles3, gles31) with the same executable patch (vq_exe_main); they run on
EGL through vpgles (KL_GLES_VPK=1).

Profiles
  rapida    deterministic sample of every relevant group: the first picture of conformance.
  completa  every case of the relevant groups (VQ_CTS_FULL=1).

Left out: groups that need what vpgles reports as absent or does not implement yet (transform
feedback, geometry/tessellation shaders, shader storage buffers / images / atomics, program
binaries, robustness contexts, the debug output tests), and the long stress/performance groups.
The exclusions are the first entries of the work list (gpu/gles/README in VPKEngine).

Usage: caselists.py <mustpass dir with gles/aosp_mustpass/main> <out dir>
Writes <out>/<profile>/NNN-<module>-<group>-PPP.txt and <out>/index.txt
("profile<TAB>file<TAB>module<TAB>group<TAB>count").
"""
import os, sys, zlib

BATCH = 400

# (module, mustpass file, group prefix after "dEQP-GLESx.", stride for "rapida", exclusions)
GROUPS = [
    ("gles2", "gles2-master.txt", "info.",                1,  []),
    ("gles2", "gles2-master.txt", "functional.prerequisite.", 1, []),
    ("gles2", "gles2-master.txt", "functional.implementation_limits.", 1, []),
    ("gles2", "gles2-master.txt", "functional.color_clear.", 2, []),
    ("gles2", "gles2-master.txt", "functional.buffer.",    4,  []),
    ("gles2", "gles2-master.txt", "functional.texture.",   8,  []),
    ("gles2", "gles2-master.txt", "functional.fbo.",       4,  []),
    ("gles2", "gles2-master.txt", "functional.shaders.",   10, []),
    ("gles2", "gles2-master.txt", "functional.uniform_api.", 6, []),
    ("gles2", "gles2-master.txt", "functional.attribute_location.", 4, []),
    ("gles2", "gles2-master.txt", "functional.vertex_arrays.", 12, []),
    ("gles2", "gles2-master.txt", "functional.draw.",      4,  []),
    ("gles2", "gles2-master.txt", "functional.fragment_ops.", 8, []),
    ("gles2", "gles2-master.txt", "functional.depth_range.", 2, []),
    ("gles2", "gles2-master.txt", "functional.polygon_offset.", 2, []),
    ("gles2", "gles2-master.txt", "functional.read_pixels.", 2, []),
    ("gles2", "gles2-master.txt", "functional.state_query.", 4, []),
    ("gles2", "gles2-master.txt", "functional.negative_api.", 2, []),
    ("gles2", "gles2-master.txt", "functional.", 20,
     ["functional.prerequisite.", "functional.implementation_limits.", "functional.color_clear.",
      "functional.buffer.", "functional.texture.", "functional.fbo.", "functional.shaders.",
      "functional.uniform_api.", "functional.attribute_location.", "functional.vertex_arrays.",
      "functional.draw.", "functional.fragment_ops.", "functional.depth_range.",
      "functional.polygon_offset.", "functional.read_pixels.", "functional.state_query.",
      "functional.negative_api.", "functional.debug_marker."]),

    ("gles3", "gles3-master.txt", "info.",                1,  []),
    ("gles3", "gles3-master.txt", "functional.prerequisite.", 1, []),
    ("gles3", "gles3-master.txt", "functional.implementation_limits.", 1, []),
    ("gles3", "gles3-master.txt", "functional.color_clear.", 2, []),
    ("gles3", "gles3-master.txt", "functional.buffer.",    6,  []),
    ("gles3", "gles3-master.txt", "functional.texture.",   12, []),
    ("gles3", "gles3-master.txt", "functional.fbo.",       8,  []),
    ("gles3", "gles3-master.txt", "functional.shaders.",   12, []),
    ("gles3", "gles3-master.txt", "functional.ubo.",       8,  []),
    ("gles3", "gles3-master.txt", "functional.uniform_api.", 8, []),
    ("gles3", "gles3-master.txt", "functional.draw.",      6,  []),
    ("gles3", "gles3-master.txt", "functional.instanced.", 4,  []),
    ("gles3", "gles3-master.txt", "functional.fragment_out.", 4, []),
    ("gles3", "gles3-master.txt", "functional.samplers.",  4,  []),
    ("gles3", "gles3-master.txt", "functional.sync.",      1,  []),
    ("gles3", "gles3-master.txt", "functional.state_query.", 6, []),
    ("gles3", "gles3-master.txt", "functional.negative_api.", 2, []),
    ("gles3", "gles3-master.txt", "functional.", 24,
     ["functional.prerequisite.", "functional.implementation_limits.", "functional.color_clear.",
      "functional.buffer.", "functional.texture.", "functional.fbo.", "functional.shaders.",
      "functional.ubo.", "functional.uniform_api.", "functional.draw.", "functional.instanced.",
      "functional.fragment_out.", "functional.samplers.", "functional.sync.",
      "functional.state_query.", "functional.negative_api.",
      "functional.transform_feedback.", "functional.occlusion_query.", "functional.multisample.",
      "functional.shader_api.program_binary.", "functional.lifetime."]),

    ("gles31", "gles31-master.txt", "info.",              1,  []),
    ("gles31", "gles31-master.txt", "functional.shaders.", 16,
     ["functional.shaders.builtin_functions.precision.", "functional.shaders.opaque_type_indexing.ssbo",
      "functional.shaders.opaque_type_indexing.atomic_counter", "functional.shaders.linkage.es31.geometry",
      "functional.shaders.linkage.es31.tessellation"]),
    ("gles31", "gles31-master.txt", "functional.texture.", 12,
     ["functional.texture.texture_buffer.", "functional.texture.border_clamp."]),
    ("gles31", "gles31-master.txt", "functional.state_query.", 8, []),
    ("gles31", "gles31-master.txt", "functional.android_extension_pack.", 1, []),
]

# Never run (not implemented by design yet; listed so the reason is visible in one place).
GLOBAL_EXCLUDE = [
    "functional.compute.shared_var", "functional.ssbo.", "functional.image_load_store.",
    "functional.atomic_counter.", "functional.geometry_shading.", "functional.tessellation.",
    "functional.program_interface_query.", "functional.separate_shader.", "functional.debug.",
    "functional.robustness.", "functional.stencil_texturing.", "functional.blend_equation_advanced.",
    "functional.sample_shading.", "functional.copy_image.", "stress.", "performance.",
]


def read_list(path):
    with open(path) as fh:
        return [l.strip() for l in fh if l.strip() and not l.startswith("#")]


def keep_sample(name, stride):
    return stride <= 1 or zlib.crc32(name.encode()) % stride == 0


def main():
    src, dst = sys.argv[1], sys.argv[2]
    base = os.path.join(src, "gles", "aosp_mustpass", "main")
    profiles = ["rapida"] + (["completa"] if os.environ.get("VQ_CTS_FULL") == "1" else [])
    index, seq, totals, cache = [], 0, {p: 0 for p in profiles}, {}
    for module, fname, prefix, stride, skip in GROUPS:
        path = os.path.join(base, fname)
        if not os.path.exists(path):
            print(f"warning: {fname} not in this mustpass", file=sys.stderr)
            continue
        if path not in cache:
            cache[path] = read_list(path)
        head = f"dEQP-{module.upper()}."
        picked = [n for n in cache[path]
                  if n.startswith(head + prefix)
                  and not any(n.startswith(head + s) for s in skip)
                  and not any(n.startswith(head + g) for g in GLOBAL_EXCLUDE)]
        label = (prefix.rstrip(".").replace(".", "-") or "all")
        for profile in profiles:
            cases = [n for n in picked if profile == "completa" or keep_sample(n, stride)]
            os.makedirs(os.path.join(dst, profile), exist_ok=True)
            for b in range(0, len(cases), BATCH):
                chunk = cases[b:b + BATCH]
                out = f"{seq:03d}-{module}-{label}-{b // BATCH + 1:03d}.txt"
                seq += 1
                with open(os.path.join(dst, profile, out), "w") as fh:
                    fh.write("\n".join(chunk) + "\n")
                index.append((profile, out, module, label, len(chunk)))
                totals[profile] += len(chunk)
    with open(os.path.join(dst, "index.txt"), "w") as fh:
        for row in index:
            fh.write("\t".join(str(x) for x in row) + "\n")
    print(", ".join(f"{p}: {n} cases" for p, n in totals.items()) + f", {len(index)} batch files")


if __name__ == "__main__":
    main()
