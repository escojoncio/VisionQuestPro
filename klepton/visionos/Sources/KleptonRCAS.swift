// RCAS («Nitidez (RCAS)» in the app): the sharpening strength the compositor's composite pass applies to
// the guest's eye (kl_reproject.c kl_rcas), 3D only (VPKEngine's UI mask keeps HUD / text untouched).
// VPKTAA is gone from the app; this is all that is left of its adapter.
import Foundation

enum KleptonRCAS {
    /// Intensity 0...1 from KL_RCAS (set by the app, per game or global); 0 = off, no cost.
    static let rcasIntensity: Float = {
        guard let v = getenv("KL_RCAS"), let f = Float(String(cString: v)) else { return 0 }
        return min(max(f, 0), 1)
    }()
    /// RCAS is fused into the composite pass whenever the intensity is above 0.
    static let rcasInCompositor: Bool = rcasIntensity > 0
}
