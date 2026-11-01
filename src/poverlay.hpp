// MGPU Bridge - POVERLAY: the standalone in-game panel, drawn by the add-on itself onto the
// swap chain's back buffer right before each present - completely independent of the ReShade
// window. A faithful port of prism-core 1.5.1's overlay (overlay.cpp): a black field, white
// ink, 1px rules, square corners; selection is inversion and the four spectral state colours
// appear only as 2px marks. Glyphs from DirectWrite (JetBrains Mono embedded, Microsoft YaHei
// UI for Chinese) in an R8 atlas. Input is polled and, while the panel is open, the game's
// reads of keys, cursor, raw input and XInput come back neutral.
//
// DATA: everything the panel shows or changes is read/ applied through the bridge's own
// adapters (gpu1 ui_state + pfinish); the prism pipeline's LiveSettings/LiveStats types do
// not exist here.
//
// RENDERING SLOT: dllmain's on_present calls poverlay::on_present() with the game's native
// queue and swap chain. The panel records its own command list and executes it on the game's
// queue, so the panel lands over the finished frame and under ReShade's own UI.
#pragma once

namespace mgpu::poverlay
{
// Called from the ReShade present event (before-event position in the runtime's present path),
// on the thread that presents. `queue_native` is the game's ID3D12CommandQueue*, `sc_native`
// its IDXGISwapChain*. Both arrive as void* so this module holds no ReShade type.
void on_present(void *queue_native, void *sc_native);

// True once the panel has armed itself on a swap chain (diagnostics).
bool running();
} // namespace mgpu::poverlay
