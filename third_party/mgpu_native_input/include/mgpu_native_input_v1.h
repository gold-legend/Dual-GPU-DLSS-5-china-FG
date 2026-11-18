#pragma once

// Fixed-layout, same-process contract between the RE2 native-input provider
// and the bridge. No Windows, COM or C++ types cross this ABI as C++ objects.
// A snapshot is borrowed until release_v1 returns. Raw GPU0 texture pointers
// are for provider-side sampling only; DLSSG receives GPU1-local copies.
// For an imported frame, the consumer returns its lease only after its neural
// queue and presents retire. That return is the same-process GPU1-consumed
// acknowledgement; the provider must not reuse the shared slot before it.
#include <stdint.h>

#define MGPU_NATIVE_INPUT_ABI 0x00010000u
#define MGPU_NATIVE_INPUT_MAGIC 0x3147524Du /* "MRG1" */
#define MGPU_NATIVE_INPUT_MAX_DIM 16384u

#define MGPU_NI_F_DX12             (1ull << 0)
#define MGPU_NI_F_CAMERA           (1ull << 1)
#define MGPU_NI_F_CAMERA_HISTORY   (1ull << 2)
#define MGPU_NI_F_MOTION_VECTORS   (1ull << 3)
#define MGPU_NI_F_DEPTH            (1ull << 4)
#define MGPU_NI_F_GAMEPLAY_KNOWN   (1ull << 5)
#define MGPU_NI_F_GAMEPLAY         (1ull << 6)
#define MGPU_NI_F_MATCHED_FRAME    (1ull << 7)
#define MGPU_NI_F_ENGINE_COPY_DONE (1ull << 8)
#define MGPU_NI_F_RESOURCE_BOUNDS  (1ull << 9)
#define MGPU_NI_F_REVERSE_Z        (1ull << 10)
#define MGPU_NI_F_CLIP_SPACE_MV    (1ull << 11)
#define MGPU_NI_F_GPU_VERIFIED     (1ull << 12)
#define MGPU_NI_F_HISTORY_RESET    (1ull << 13)
#define MGPU_NI_F_PIXEL_HISTORY    (1ull << 14)
#define MGPU_NI_F_TRACKED_COPY     (1ull << 15)
#define MGPU_NI_F_CROSS_ADAPTER    (1ull << 16)
#define MGPU_NI_F_COLOR_MATCHED    (1ull << 17)

#define MGPU_NATIVE_INPUT_SAMPLE_COUNT 1600u /* 5x5 tiles of 8x8 pixels. */
#define MGPU_NI_SAMPLE_MV_HALF         (1u << 0)
#define MGPU_NI_SAMPLE_DEPTH_F32       (1u << 1)
#define MGPU_NI_SAMPLE_DEPTH_U24       (1u << 2)
#define MGPU_NI_SAMPLE_DEPTH_U16       (1u << 3)
#define MGPU_NI_SAMPLE_TRACKED_STATES  (1u << 4)

#pragma pack(push, 1)
typedef struct mgpu_native_transfer_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t heap_handle;       /* Shared cross-adapter ID3D12Heap HANDLE. */
    uint64_t fence_handle;      /* Shared cross-adapter ID3D12Fence HANDLE. */
    uint64_t fence_value;       /* Signal is already queued on the engine queue. */
    uint64_t heap_bytes;
    uint64_t slot_bytes;
    uint64_t mv_offset;         /* Within this snapshot's slot. */
    uint64_t depth_offset;
    uint64_t mv_bytes;
    uint64_t depth_bytes;
    uint32_t mv_row_pitch;
    uint32_t depth_row_pitch;
    uint32_t mv_rows;
    uint32_t depth_rows;
    uint32_t mv_copy_format;    /* Actual footprint format, not a typeless guess. */
    uint32_t depth_copy_format;
} mgpu_native_transfer_v1;

typedef struct mgpu_native_input_v1 {
    uint32_t sequence;          /* Odd while the provider writes the slot. */
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t slot_index;
    uint64_t frame_id;          /* Provider snapshot number, NOT neural cadence. */
    uint64_t generation;
    uint64_t qpc;
    uint64_t game_frame;        /* Exact bridge color/seal number. */
    uint64_t previous_game_frame; /* Previous successful NEURAL OUTPUT, not previous game frame. */
    uint64_t flags;
    uint64_t reason_mask;
    uint64_t game_luid;
    uint32_t process_id;
    uint32_t camera_type;
    uint32_t mv_width, mv_height, mv_format;
    uint32_t depth_width, depth_height, depth_format;
    uint32_t color_width, color_height;
    uint32_t cookie;
    uint32_t reserved0;
    void* mv_resource;          /* GPU0 provider clone; never pass this to NGX. */
    void* depth_resource;
    float view_to_clip[16];     /* GLM column-major, before TAA jitter. */
    float clip_to_view[16];
    float clip_to_prev_clip[16]; /* Exact previous neural-output camera history. */
    float prev_clip_to_clip[16];
    float camera_position[3];
    float camera_right[3];
    float camera_up[3];
    float camera_forward[3];
    float camera_near;
    float camera_far;
    float camera_fov_y;         /* Radians, derived from the real projection. */
    float camera_aspect;
    float mv_scale_x;           /* Do not use until pixels validate the encoding. */
    float mv_scale_y;
    float jitter_x, jitter_y;
    mgpu_native_transfer_v1 transfer;
    uint64_t reserved1[4];
} mgpu_native_input_v1;

typedef struct mgpu_native_input_sample_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t count;
    uint32_t flags;
    uint64_t frame_id;
    uint64_t generation;
    uint32_t mv_state;
    uint32_t depth_state;
    uint16_t x[MGPU_NATIVE_INPUT_SAMPLE_COUNT];
    uint16_t y[MGPU_NATIVE_INPUT_SAMPLE_COUNT];
    uint16_t mv_raw[MGPU_NATIVE_INPUT_SAMPLE_COUNT * 2u];
    uint32_t depth_raw[MGPU_NATIVE_INPUT_SAMPLE_COUNT];
} mgpu_native_input_sample_v1;

typedef struct mgpu_native_input_api_v1 {
    uint32_t magic;
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t slot_count;
    uint64_t (*acquire_v1)(uint64_t minimum_frame, mgpu_native_input_v1* out, uint32_t out_size);
    void (*release_v1)(const mgpu_native_input_v1* frame);
    uint64_t (*generation_v1)(void);
    const char* (*last_reason_v1)(void);
    int (*sample_v1)(const mgpu_native_input_v1* frame, mgpu_native_input_sample_v1* out, uint32_t out_size);
    uint64_t (*acquire_for_color_v1)(uint64_t game_frame, mgpu_native_input_v1* out, uint32_t out_size);
    /* Called when a color seal is recorded. Zeros deactivate native capture. */
    uint64_t (*bind_color_v1)(uint64_t game_frame, uint64_t previous_game_frame, uint64_t seal_qpc);
} mgpu_native_input_api_v1;
#pragma pack(pop)

typedef const mgpu_native_input_api_v1* (__cdecl *mgpu_get_native_input_v1_fn)(void);

// A provider must not set GPU_VERIFIED itself. Only the bridge can do that,
// after matching readback pixels against exact neural-frame camera matrices.
static inline uint64_t mgpu_native_input_missing(uint64_t flags, int gameplay_only) {
    uint64_t need = MGPU_NI_F_DX12 | MGPU_NI_F_CAMERA | MGPU_NI_F_CAMERA_HISTORY |
        MGPU_NI_F_MOTION_VECTORS | MGPU_NI_F_DEPTH | MGPU_NI_F_GAMEPLAY_KNOWN |
        MGPU_NI_F_MATCHED_FRAME | MGPU_NI_F_ENGINE_COPY_DONE | MGPU_NI_F_RESOURCE_BOUNDS |
        MGPU_NI_F_TRACKED_COPY | MGPU_NI_F_REVERSE_Z | MGPU_NI_F_CLIP_SPACE_MV |
        MGPU_NI_F_GPU_VERIFIED | MGPU_NI_F_CROSS_ADAPTER | MGPU_NI_F_COLOR_MATCHED;
    if (gameplay_only) need |= MGPU_NI_F_GAMEPLAY;
    return (~flags) & need;
}

static inline int mgpu_native_input_transfer_valid(const mgpu_native_input_v1* s) {
    if (s == 0) return 0;
    const mgpu_native_transfer_v1* t = &s->transfer;
    if (t->struct_size != (uint32_t)sizeof(*t) || t->abi_version != MGPU_NATIVE_INPUT_ABI ||
        t->heap_handle == 0 || t->fence_handle == 0 || t->fence_value == 0 ||
        t->heap_bytes == 0 || t->slot_bytes == 0 || t->slot_bytes > t->heap_bytes / 3u ||
        (t->mv_offset & 511ull) != 0 || (t->depth_offset & 511ull) != 0 ||
        t->mv_row_pitch == 0 || t->depth_row_pitch == 0 ||
        t->mv_rows != s->mv_height || t->depth_rows != s->depth_height ||
        (uint64_t)t->mv_row_pitch * t->mv_rows > t->mv_bytes ||
        (uint64_t)t->depth_row_pitch * t->depth_rows > t->depth_bytes ||
        t->mv_offset > t->slot_bytes || t->mv_bytes > t->slot_bytes - t->mv_offset ||
        t->depth_offset < t->mv_offset + t->mv_bytes ||
        t->depth_offset > t->slot_bytes || t->depth_bytes > t->slot_bytes - t->depth_offset) return 0;
    if (t->mv_copy_format != 34u || s->mv_format != 34u || /* DXGI_FORMAT_R16G16_FLOAT. */
        (t->depth_copy_format != 10u && t->depth_copy_format != 41u && t->depth_copy_format != 56u)) return 0;
    /* Source texture and linear copy must describe the SAME depth encoding. */
    if ((t->depth_copy_format == 10u && s->depth_format != 2u && s->depth_format != 40u && s->depth_format != 41u) ||
        (t->depth_copy_format == 41u && s->depth_format != 45u && s->depth_format != 47u) ||
        (t->depth_copy_format == 56u && s->depth_format != 54u && s->depth_format != 55u)) return 0;
    const uint64_t mv_tight = (uint64_t)s->mv_width * 4u;
    const uint64_t depth_tight = (uint64_t)s->depth_width *
        (t->depth_copy_format == 56u ? 2u : 4u);
    if (mv_tight > t->mv_row_pitch || depth_tight > t->depth_row_pitch ||
        t->mv_row_pitch % 256u != 0u || t->depth_row_pitch % 256u != 0u) return 0;
    const uint64_t mv_total = (uint64_t)(t->mv_rows - 1u) * t->mv_row_pitch + mv_tight;
    const uint64_t depth_total = (uint64_t)(t->depth_rows - 1u) * t->depth_row_pitch + depth_tight;
    return mv_total <= t->mv_bytes && depth_total <= t->depth_bytes;
}

static inline int mgpu_native_input_bounds_valid(const mgpu_native_input_v1* s) {
    if (s == 0 || s->struct_size != (uint32_t)sizeof(*s) ||
        s->abi_version != MGPU_NATIVE_INPUT_ABI || s->mv_resource == 0 || s->depth_resource == 0 ||
        s->frame_id == 0 || s->generation == 0 || s->cookie >= 3u ||
        s->slot_index != s->cookie) return 0;
    if (s->mv_width == 0 || s->mv_height == 0 || s->depth_width == 0 || s->depth_height == 0 ||
        s->color_width == 0 || s->color_height == 0 || s->mv_width > MGPU_NATIVE_INPUT_MAX_DIM ||
        s->mv_height > MGPU_NATIVE_INPUT_MAX_DIM || s->depth_width > MGPU_NATIVE_INPUT_MAX_DIM ||
        s->depth_height > MGPU_NATIVE_INPUT_MAX_DIM) return 0;
    if (s->mv_width != s->depth_width || s->mv_height != s->depth_height ||
        s->mv_width != s->color_width || s->mv_height != s->color_height) return 0;
    return (s->flags & MGPU_NI_F_CROSS_ADAPTER) == 0 || mgpu_native_input_transfer_valid(s);
}

static inline int mgpu_native_input_finite_matrix(const float* m) {
    if (m == 0) return 0;
    for (int i = 0; i < 16; ++i) {
        /* No <cmath> dependency in this C ABI; NaNs fail both comparisons. */
        if (!(m[i] >= -1.0e30f && m[i] <= 1.0e30f)) return 0;
    }
    return 1;
}
