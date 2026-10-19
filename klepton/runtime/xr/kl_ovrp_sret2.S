// sret capture for the legacy pose queries of kl_ovrp_input.c / kl_ovrp_render.c
// (same technique and same reason as kl_ovrp_sret.S: the caller's destination
// pointer arrives in x8 and every call clobbers it, so it is moved, in assembly,
// into the first argument register the C implementation does not use before
// anything else runs). The real plugin returns a 28-byte struct (ovrpPosef-shaped,
// also for velocity and acceleration: orientation identity, vector in Position)
// through x8 for all of these; read off the disassembly of nine OVRPlugin builds
// (tools/ovrp_abi.py), identical in every version.
//
//   klovrp_GetNodePose_impl(node=w0, out=x1)        (also Velocity, Acceleration)
//   klovrp_GetNodePose2_impl(step=w0, node=w1, out=x2)   (also Velocity2, Acceleration2)
//   klovrp_GetEyePose_impl(eye=w0, out=x1)          (also Velocity, Acceleration)
//   klovrp_GetTrackerPose_impl(tracker=w0, out=x1)
//   klovrp_GetTrackingCalibratedOrigin_impl(out=x0)

    .text
    .globl  _klovrp_GetNodePose_entry
_klovrp_GetNodePose_entry:
    mov     x1, x8
    b       _klovrp_GetNodePose_impl

    .globl  _klovrp_GetNodeVelocity_entry
_klovrp_GetNodeVelocity_entry:
    mov     x1, x8
    b       _klovrp_GetNodeVelocity_impl

    .globl  _klovrp_GetNodeAcceleration_entry
_klovrp_GetNodeAcceleration_entry:
    mov     x1, x8
    b       _klovrp_GetNodeAcceleration_impl

    .globl  _klovrp_GetNodePose2_entry
_klovrp_GetNodePose2_entry:
    mov     x2, x8
    b       _klovrp_GetNodePose2_impl

    .globl  _klovrp_GetNodeVelocity2_entry
_klovrp_GetNodeVelocity2_entry:
    mov     x2, x8
    b       _klovrp_GetNodeVelocity2_impl

    .globl  _klovrp_GetNodeAcceleration2_entry
_klovrp_GetNodeAcceleration2_entry:
    mov     x2, x8
    b       _klovrp_GetNodeAcceleration2_impl

    .globl  _klovrp_GetTrackerPose_entry
_klovrp_GetTrackerPose_entry:
    mov     x1, x8
    b       _klovrp_GetTrackerPose_impl

    .globl  _klovrp_GetEyePose_entry
_klovrp_GetEyePose_entry:
    mov     x1, x8
    b       _klovrp_GetEyePose_impl

    .globl  _klovrp_GetEyeVelocity_entry
_klovrp_GetEyeVelocity_entry:
    mov     x1, x8
    b       _klovrp_GetEyeVelocity_impl

    .globl  _klovrp_GetEyeAcceleration_entry
_klovrp_GetEyeAcceleration_entry:
    mov     x1, x8
    b       _klovrp_GetEyeAcceleration_impl

    .globl  _klovrp_GetTrackingCalibratedOrigin_entry
_klovrp_GetTrackingCalibratedOrigin_entry:
    mov     x0, x8
    b       _klovrp_GetTrackingCalibratedOrigin_impl
