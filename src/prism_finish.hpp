#pragma once

// MGPU Bridge - PRISM-FINISH: the prism-core grade + guard pass, ported onto the
// bridge's own pass chain (off by default).
//
// WHAT THIS IS FOR
//
// The pivot build (2026-10-29): mgpu 0.4 stays the transport, and the prism-core
// 1.5.1 menu features that made the single-card picture worth looking at are
// grafted onto it. This module carries the two that need new GPU code:
//
//   colour (the Look presets): prism's Oklab grade pipeline (look_hlsl.h, Look 1-4;
//     Look 0 = identity). Grades the frame DLSS-NR produced, in place, on GPU 1,
//     after the last pass's evaluate.
//   character guard (the chroma/skin guard): prism's CbCr skin detector + high-saturation
//     detector (pipeline.cpp ~1089). Where the mask fires, the neural answer is
//     blended back toward the original frame the network was handed, so faces and
//     already-vivid colours keep the game's own rendering.
//
// WHAT IT IS NOT
//
//   - Not the prism netscale cadence path. The "run the network at a scaled
//     resolution every frame" option is deliberately NOT ported (owner decision,
//     2026-10-29). mgpu's own NRS (R299) is untouched and unaffected.
//   - Not the e-field / WfApply architecture. That was sealed with the old main
//     line. The guard here is the finished, standalone math from prism 1.5.1.
//   - Not Look 5 (custom): the full grade-page UI is its own change. Asking for
//     look 5 falls back to look 0 with one log line.
//
// THE CONTRACT (same rules as residual.hpp, which this module is modelled on)
//
//   - OPTIONAL MODULE, keys in mgpu.ini, read at arm:
//       Finish        1: build the pass. Absent: no key read beyond this one, no
//                     kernel, no dispatch, no log line - the frame path is the
//                     published one, byte for byte.
//       FinishLook    0..4, default 0 (default, identity). 1 faithful 2 vivid
//                     3 muted 4 rich. Live-changeable from the panel.
//       FinishGuard   0.0..1.0, default 1.0 (character guard strength; 0 = off).
//                     Live-changeable from the panel.
//       FinishGamma   0 or 2.0..2.6, default 2.2: the display-code <-> scene-linear
//                     wrap around the grade. prism single-card graded the game's
//                     LINEAR frame; the bridge's NR output is display code, so the
//                     grade is bracketed with pow 2.2 / pow 1/2.2. 0 disables the
//                     wrap (grade on code values directly). One variable, tuned by
//                     looking at the vivid preset against the split view.
//   - Runs on the LAST pass's output only, after that evaluate, inside the
//     stream's command list, on the bridge thread - the same slot residual::after
//     uses, and mutually exclusive with it (theirs wins, same as NRS).
//   - In-place: reads and writes the same UAV texel from the same thread. The
//     original frame (the network's input) is read as an SRV. Both textures are
//     full-size, same format, by construction.
//   - Holds no gpu1_context type and no ReShade type: gpu1_context hands it raw
//     D3D12 objects. A compile or allocation failure leaves the module OFF and
//     never fails the arm.
//   - OFF COSTS: one file read at arm, then one relaxed load per evaluate.

#include <windows.h>
#include <d3d12.h>

#include "prism_grade.h"   // GradeValues / kGradeSlots (the custom-look table)

namespace mgpu::pfinish
{
// At arm, after the stream's textures exist and after the R316 block has run.
// `residual_on` is that module's verdict (true = theirs is on, ours stays off).
// Reads the keys; when on, compiles the kernel and builds the descriptor heap.
// False (and off) otherwise. Never fails the arm.
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt, bool residual_on);

// One relaxed load.
bool active();

// Records the finish pass on `cl`: grades `out` in place (UAV), reading the
// original frame `in` (SRV, already NON_PIXEL_SHADER_RESOURCE). Both are the
// native extent, same format. `parity` picks the descriptor slot pair.
// No-op unless active(). Call ONLY on the last pass of a successful evaluate.
// grade_only is for PSYNC's already-guarded output: run Look/custom Grade only,
// without applying the character guard or HDR cap a second time.
void apply(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *out, ID3D12Resource *in,
           bool grade_only = false);

// Live panel control. Values are picked up by the very next dispatch.
void set_look(unsigned look);      // 0..5 (5 = the custom grade, Look 5)
void set_guard(float guard);       // 0..1, clamped
unsigned look();                   // what the next dispatch will use
float guard();

// ---- the custom grade table (Look 5), ported from prism-core 1.5.1's grade.h ----
// Indices are prism::Grade (prism_grade.h, 66 controls). Values are in the table's own units.
// THREE slots like the prism desk (方案 1/2/3); set_grade_value et al. act on the ACTIVE slot.
void set_grade_value(int index, float v);
float grade_value(int index);
void reset_grade();      // active slot back to defaults
void save_grade();       // write the active slot's non-default values (call on slider release)
int   grade_slot();                        // the active slot 0..2
void  set_grade_slot(int slot);            // switch the active slot (the b1 buffer repacks)
prism::GradeValues grade_snapshot(int slot);                 // a copy of one slot
void  set_grade_values(int slot, const prism::GradeValues &); // replace a whole slot
void  save_grade_slot(int slot);           // persist one slot

// From stream_release, when the GPU no longer uses the stream's resources.
void release();
}
