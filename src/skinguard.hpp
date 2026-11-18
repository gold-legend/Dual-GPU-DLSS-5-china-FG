// skinguard.hpp - [OURS] post-pass skin gating for the multi-pass cascade.
//
// WHAT THIS IS. With Passes>=2 the cascade refines pass 1's output through the
// later passes, and the model's skin/structure split cannot keep the face at
// pass-1 strength while the world gets the full cascade. This module adds a
// bridge-side gate AFTER the pass loop: it blends per pixel between the pass-1
// output (kept on skin) and the final pass output (kept everywhere else):
//
//     out = m * pass1 + (1 - m) * pass_final
//
// m is a temporally smoothed skin mask computed from the native frame's Cb-Cr
// chroma locus - skin tones cluster tightly in chroma regardless of lighting,
// which is what face detectors have exploited for decades. False positives
// (wood, sand) are harmless: those pixels just keep the pass-1 look.
//
// WHERE IT RUNS. compose() is called once per consumed frame after the whole
// pass loop, in the plain full-frame shape only (no SR, no foveated, no VR,
// no subrect, no CostCurve) - the same shape list the residual module refuses.
// The blend is written IN PLACE into the pass-1 output texture and the caller
// points nr_final at that same texture. No foreign resource enters the present
// path. Two and three pass cascades are supported: the inputs are copied to
// private buffers before drawing, and compose still refuses any actual alias.
//
// TEXTURE STATES at the compose() call (contract with the caller):
//   native - NON_PIXEL_SHADER_RESOURCE (pass 0 left it there), read-only
//   target - the pass-1 output texture, in UNORDERED_ACCESS; read AND written
//            in place through a single UAV (legal: one UAV, no SRV alias)
//   final  - the final pass output texture, UNORDERED_ACCESS, read-only SRV
// target and final are both returned to UNORDERED_ACCESS before return, which
// is the state they already rest in - the dispatch changes no resting state.

#pragma once

#include <d3d12.h>

namespace mgpu::skinguard
{
// Reads nothing; stores the tuning. Called once before arm().
// cb/cr: skin locus centre in 8-bit Cb/Cr units (0..255, 128 = neutral).
// rin/rout: full-protect / no-protect radii around the locus, same units.
// temporal: fraction of the previous frame's mask kept (0..0.9).
void configure(float cb, float cr, float rin, float rout, float temporal);

// Called from stream_release, when the GPU no longer uses the stream's resources.
void release();

// At arm time, after the stream's textures exist. `mode` is the SkinGuard ini
// value: 0 = off, 1 = full masked blend, 2 = A/B DIAGNOSTIC - no dispatch,
// plain CopyResource(final -> target): mode 2 isolates whether a failure
// comes from the in-place routing (2 also faults) or from the compute
// dispatch itself (2 clean, 1 faults).
// `width`/`height` are the stream's own. Builds the mask ping-pong pair, the
// descriptor heap, the root signature and the PSO. Never fails the arm - on
// any failure it stays off.
// `shape_refusal` names the first arm-time setting the guard cannot run with
// (nullptr = none), same convention as the residual module.
bool arm(int mode, ID3D12Device *dev, unsigned width, unsigned height,
         DXGI_FORMAT fmt, const char *shape_refusal);

// One relaxed load. active() is the live enable used by the render path.
bool active();
// The Prism menu toggles the already-armed module without rebuilding NGX.
// If the current shape cannot support the module, this only leaves it off.
void set_enabled(bool on);

// The frame blend. See the state contract above. `parity` is the tex_in ring
// parity of this frame; the mask history ping-pongs on the same parity.
// `target` is the pass-1 output texture (read-write in place via a buffer
// roundtrip); `final` is the last pass output (read-only). `native_rest` is
// the resting state tex_in[parity] is in when this runs (the caller's
// s.tex_in_rest) - the blend snapshots it with a footprint copy. On success
// the CALLER sets s.nr_final = target - this module returns no texture of
// its own.
bool compose(ID3D12GraphicsCommandList *cl, unsigned parity,
             ID3D12Resource *native, ID3D12Resource *target, ID3D12Resource *final,
             unsigned w, unsigned h, D3D12_RESOURCE_STATES native_rest);
}
