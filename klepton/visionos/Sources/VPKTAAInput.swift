// VPKTAA inputs: what KleptonTAA gathers per eye and per guest frame for VPKEngine's native
// VPKTAA pass (vpvk_taa_native_encode; the only VPKTAA). It is copied into kl_vulkan_taa_native_frame
// (KleptonTAA.nativeFrame). The jitter is VPKEngine's own (Halton(2,3), applied to the viewport),
// so none is carried here.
//   color        the guest's picture: .type2DArray texture, `slice` = layer of this eye.
//   flipY        true when texture row 0 is the TOP of the picture (Vulkan, Metal).
//   tangents     l r t b (all positive) of the UNjittered frustum.
//   crop/cropOff the rect of `color` that holds the picture, as fractions of the texture.
//   orientation  world-from-eye rotation of the camera this eye was rendered with.
//   position     that camera's position in the same world space, metres.
//   depth        optional: the depth buffer the picture was rendered with (single-sample or
//                MULTISAMPLED) + reversedZ + near/far of the projection (far 0 = infinite).
//   motion       optional: OBJECT motion vectors (as Meta AppSW); `scale` maps a texel to
//                (current - previous) in crop-uv units.
import Metal
import simd
import Foundation

struct VPKTAADepthInput {
    var texture: MTLTexture
    var slice: Int
    var reversedZ: Bool
    var near: Float
    var far: Float          // 0 = infinite
    /// true: the texture covers the crop rect only (e.g. AppSW depth at motion resolution);
    /// false: it has the colour texture's layout (any resolution).
    var mapsCrop: Bool = false
}

struct VPKTAAMotionInput {
    var texture: MTLTexture
    var slice: Int
    var scale: SIMD2<Float>
    /// true: the texture covers the crop rect only (Meta AppSW: motion vectors are rendered at
    /// MotionVectorTextureSize with the eye's own viewport); false: the colour's layout.
    var mapsCrop: Bool = true
}

struct VPKTAAEyeInput {
    var color: MTLTexture
    var slice: Int
    var flipY: Bool
    var tangents: SIMD4<Float>
    var crop: SIMD2<Float> = SIMD2<Float>(1, 1)
    var cropOff: SIMD2<Float> = SIMD2<Float>(0, 0)
    var orientation: simd_quatf
    var position: SIMD3<Float>
    var depth: VPKTAADepthInput? = nil
    var motion: VPKTAAMotionInput? = nil
}
