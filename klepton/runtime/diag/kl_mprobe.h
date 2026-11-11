// Managed-side probe: ask Unity what it thinks, in Unity's own words.
//
// Every input measurement so far has been taken on the *native* side of the
// boundary — what ovrp handed libunity, what libunity's joystick fill wrote.
// That leaves the last hop unmeasured: whether any of it survives into
// `UnityEngine.Input`, which is where this title actually reads controllers
//  (the InputManager binds TriggerLeftHand/TriggerRightHand to joystick axes 8 and 9). "Where Unity learns how many joysticks exist" was
// the named gap; `Input.GetJoystickNames()` answers it directly.
//
// libil2cpp exports the standard embedding API, so no address table or
// hand-built calling convention is needed: domain -> assembly -> image ->
// class -> MethodInfo -> il2cpp_runtime_invoke. The runtime does class init,
// argument marshalling and boxing itself, which is why this is the robust
// way in rather than calling compiled method bodies directly.
//
// Diagnostic only, and off unless KL_PROBE_INPUT is set.
#ifndef KL_MPROBE_H
#define KL_MPROBE_H

// Called from the frame pump with the frame index. Prints a line every
// KL_PROBE_EVERY frames (default 120). No-op unless KL_PROBE_INPUT=1.
void kl_mprobe_tick(unsigned frame);

// kl_ovrp.c: the guest's non-eye layer count changed between zero and non-zero (a menu opened or closed). The next tick
// logs what OVRInput.GetConnectedControllers / GetActiveController say (diagnosis only; any thread).
void kl_mprobe_note_menu(int overlays_before, int overlays_after);

// The last "Canvas=active/all" of the scene census ("[mprobe fN] scene: ... Canvas=a/b (active/all)"): 1 when one was taken (then *frame is its
// frame), 0 otherwise (the probe is off, or the game has no Canvas type). Any thread; -1 in a count = not counted.
int kl_mprobe_last_canvas(int *active, int *all, int *frame);

#endif
