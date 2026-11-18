// B1 every-frame local NR: GPU0 owns NGX, GPU1 owns same-frame composition.
// Included inside gpu1_context.cpp's anonymous namespace.

static stream_state &main_nr_str()
{
    static stream_state m;
    return m;
}

static bool main_nr_faulted = false;

static const char *b1_stage_name(unsigned i)
{
    static const char *const names[5] = {
        "native-copy-in", "NR-P1+resize", "NR-P2+resize", "NR-P3+resize", "answer-copy-out"
    };
    return names[(i < 5u) ? i : 0u];
}

static void b1_ts_collect(stream_state &m)
{
    if (!m.b1_ts_pending || m.b1_ts_read == nullptr || m.b1_ts_freq == 0 ||
        m.b1_ts_marks < 2u || m.b1_ts_marks > 6u) return;
    const UINT64 *v = nullptr;
    const D3D12_RANGE range{0, (SIZE_T)m.b1_ts_marks * 8u};
    if (SUCCEEDED(m.b1_ts_read->Map(0, &range, (void **)&v)) && v != nullptr)
    {
        const unsigned spans = m.b1_ts_marks - 1u;
        for (unsigned i = 0; i < spans && i < 5u; ++i)
        {
            if (v[i + 1u] < v[i]) continue;
            const double ms = (double)(v[i + 1u] - v[i]) * 1000.0 / (double)m.b1_ts_freq;
            const unsigned stage = (i + 1u == spans) ? 4u : i; // answer-copy-out follows the active pass count
            m.b1_ts_sum[stage] += ms;
            if (m.b1_ts_n[stage] == 0ull || ms < m.b1_ts_min[stage]) m.b1_ts_min[stage] = ms;
            if (ms > m.b1_ts_max[stage]) m.b1_ts_max[stage] = ms;
            ++m.b1_ts_n[stage];
        }
        m.b1_ts_read->Unmap(0, nullptr);
    }
    m.b1_ts_pending = false;
    if (m.b1_ts_n[0] != 0ull && m.b1_ts_n[0] % 120ull == 0ull)
    {
        char line[760];
        int n = snprintf(line, sizeof line, "[MGPU][B1][GPU0-STAGE] GPU ms mean/min/max");
        for (unsigned i = 0; i < 5u && n > 0 && (size_t)n < sizeof line; ++i)
            if (m.b1_ts_n[i] != 0ull)
                n += snprintf(line + n, sizeof line - (size_t)n, " | %s %.3f/%.3f/%.3f",
                              b1_stage_name(i), m.b1_ts_sum[i] / (double)m.b1_ts_n[i],
                              m.b1_ts_min[i], m.b1_ts_max[i]);
        mgpu::diag::info(line);
    }
}

// Queue a GPU-side wait before any command that reuses frame fi's ring slot.
// CPU recording may continue, but the game queue cannot overwrite the shared
// slot until GPU1 has finished reading its previous occupant. The caller holds
// no blocking CPU wait and the consumer can still release s.cs as before.
static bool local_nr_gate(stream_state &s, ID3D12CommandQueue *gq,
                          unsigned long long fi)
{
    if (!s.local_nr) return true;
    const unsigned long long target =
        mgpu::b1::ring_release_target(fi, stream_state::RING);
    if (target == 0 || target <= s.local_nr_gated_through) return true;
    if (gq == nullptr || s.local_nr_consumed_g == nullptr)
    {
        ++s.local_nr_backpressure_failures;
        return false;
    }

    // Avoid adding a queue wait when the consumer has already retired the slot.
    if (s.local_nr_consumed_g->GetCompletedValue() >= target)
    {
        s.local_nr_gated_through = target;
        return true;
    }

    const HRESULT hr = gq->Wait(s.local_nr_consumed_g, (UINT64)target);
    if (FAILED(hr))
    {
        ++s.local_nr_backpressure_failures;
        char line[300];
        snprintf(line, sizeof line,
                 "[MGPU][B1] ring backpressure Wait failed f=%llu retire=%llu hr=0x%08X; frame not sealed",
                 fi, target, (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }
    s.local_nr_gated_through = target;
    s.local_nr_wait_target.store(target, std::memory_order_release);
    ++s.local_nr_backpressure_waits;
    if (s.local_nr_backpressure_waits == 1 || s.local_nr_backpressure_waits % 120ull == 0ull)
    {
        char line[260];
        snprintf(line, sizeof line,
                 "[MGPU][B1] ring backpressure active: f=%llu waits for GPU1 consumed f=%llu (wait #%llu)",
                 fi, target, s.local_nr_backpressure_waits);
        mgpu::diag::info(line);
    }
    return true;
}

// A queue Wait does not bound CPU recording. Before gup_cpu or a mid-scene
// slot can be reused, block the producer CPU on the same consumed fence. The
// caller must hold neither s.cs nor mvec_cs: GPU1 releases s.cs across its own
// fence wait, and this wait is what allows that consumer to advance.
static bool local_nr_cpu_gate(stream_state &s, unsigned long long fi)
{
    if (!s.local_nr) return true;
    const unsigned long long target =
        mgpu::b1::ring_release_target(fi, stream_state::RING);
    if (target == 0 || target <= s.local_nr_cpu_gated_through) return true;
    if (s.local_nr_cpu_stop.load(std::memory_order_acquire)) return false;
    ID3D12Fence *const fence = s.local_nr_consumed_g;
    HANDLE const event = s.local_nr_cpu_event;
    if (fence == nullptr || event == nullptr)
    {
        ++s.local_nr_cpu_failures;
        return false;
    }
    if (fence->GetCompletedValue() >= (UINT64)target)
    {
        s.local_nr_cpu_gated_through = target;
        return true;
    }

    const HRESULT hr = fence->SetEventOnCompletion((UINT64)target, event);
    if (FAILED(hr) && fence->GetCompletedValue() < (UINT64)target)
    {
        ++s.local_nr_cpu_failures;
        char line[300];
        snprintf(line, sizeof line,
                 "[MGPU][B1] CPU ring gate SetEventOnCompletion failed f=%llu retire=%llu hr=0x%08X; frame not sealed",
                 fi, target, (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    ++s.local_nr_cpu_waits;
    const DWORD wr = WaitForSingleObject(event, 5000);
    const bool complete = fence->GetCompletedValue() >= (UINT64)target;
    if (wr != WAIT_OBJECT_0 || !complete)
    {
        if (!s.local_nr_cpu_stop.load(std::memory_order_acquire))
        {
            ++s.local_nr_cpu_failures;
            char line[340];
            snprintf(line, sizeof line,
                     "[MGPU][B1] CPU ring gate failed f=%llu retire=%llu wait=%lu completed=%llu; frame not sealed",
                     fi, target, (unsigned long)wr,
                     (unsigned long long)fence->GetCompletedValue());
            mgpu::diag::error(line);
        }
        return false;
    }
    s.local_nr_cpu_gated_through = target;
    if (s.local_nr_cpu_waits == 1 || s.local_nr_cpu_waits % 120ull == 0ull)
    {
        char line[260];
        snprintf(line, sizeof line,
                 "[MGPU][B1] CPU ring backpressure active: f=%llu retired through f=%llu (wait #%llu)",
                 fi, target, s.local_nr_cpu_waits);
        mgpu::diag::info(line);
    }
    return true;
}

void local_nr_release()
{
    stream_state &m = main_nr_str();
    stream_state &s = str();
    // Invalidate every published answer before draining or releasing storage.
    // A later stream generation must never accept this generation's pixels.
    ++s.local_nr_generation;
    for (unsigned i = 0; i < stream_state::RING; ++i)
    {
        s.local_nr_slot_frame[i].store(0ull, std::memory_order_release);
        s.local_nr_slot_generation[i].store(0ull, std::memory_order_release);
    }
    mgpu::diag::info("[MGPU][B1] GPU0 local NR release begin; all frame/generation tags invalidated");

    // A private submission can still be waiting for a game frame that WM_CLOSE
    // will never publish. Retire that wait before draining our own queue.
    if (m.nq != nullptr && m.nf != nullptr)
    {
        if (m.ninput != nullptr && m.nr_submitted != 0 &&
            m.ninput->GetCompletedValue() < (UINT64)m.nr_submitted)
        {
            (void)m.ninput->Signal((UINT64)m.nr_submitted);
        }
        const UINT64 fin = m.nf_value + 1;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = m.nq->Signal(m.nf, fin);
        DWORD wr = WAIT_OBJECT_0;
        if (SUCCEEDED(hr))
        {
            m.nf_value = fin;
            if (ev != nullptr && m.nf->GetCompletedValue() < fin)
            {
                hr = m.nf->SetEventOnCompletion(fin, ev);
                if (SUCCEEDED(hr)) wr = WaitForSingleObject(ev, 2000);
            }
        }
        if (ev != nullptr) CloseHandle(ev);
        if (FAILED(hr) || wr != WAIT_OBJECT_0)
        {
            m.nr_ok = false;
            mgpu::diag::error("[MGPU][B1] GPU0 private queue did not drain within 2 s; resources are left for process cleanup");
            return;
        }
    }

    if (!main_nr_faulted && m.nr_release != nullptr)
        for (int i = (int)stream_state::MAX_PASSES - 1; i >= 0; --i)
            if (m.nr_handle[i] != nullptr)
            {
                (void)m.nr_release(m.nr_handle[i]);
                m.nr_handle[i] = nullptr;
            }
    if (!main_nr_faulted && m.ngx_destroy_params != nullptr && m.nr_params != nullptr)
        (void)m.ngx_destroy_params(m.nr_params);
    for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i) m.nr_handle[i] = nullptr;
    m.nr_params = nullptr;
    m.nr_eval = nullptr;
    m.nr_release = nullptr;
    m.ngx_destroy_params = nullptr;
    m.nr_ok = false;

    b1_ts_collect(m);
    if (m.b1_ts_n[0] != 0ull)
    {
        char line[760];
        int n = snprintf(line, sizeof line, "[MGPU][B1][GPU0-STAGE][SUMMARY] GPU ms mean/min/max");
        for (unsigned i = 0; i < 5u && n > 0 && (size_t)n < sizeof line; ++i)
            if (m.b1_ts_n[i] != 0ull)
                n += snprintf(line + n, sizeof line - (size_t)n, " | %s n=%llu %.3f/%.3f/%.3f",
                              b1_stage_name(i), m.b1_ts_n[i], m.b1_ts_sum[i] / (double)m.b1_ts_n[i],
                              m.b1_ts_min[i], m.b1_ts_max[i]);
        mgpu::diag::info(line);
    }
    if (m.b1_ts_read != nullptr) { m.b1_ts_read->Release(); m.b1_ts_read = nullptr; }
    if (m.b1_ts_heap != nullptr) { m.b1_ts_heap->Release(); m.b1_ts_heap = nullptr; }
    if (m.nrs_pso_down != nullptr) { m.nrs_pso_down->Release(); m.nrs_pso_down = nullptr; }
    if (m.nrs_pso_res  != nullptr) { m.nrs_pso_res->Release();  m.nrs_pso_res = nullptr; }
    if (m.nrs_rs_down  != nullptr) { m.nrs_rs_down->Release();  m.nrs_rs_down = nullptr; }
    if (m.nrs_rs_res   != nullptr) { m.nrs_rs_res->Release();   m.nrs_rs_res = nullptr; }
    if (m.nrs_heap     != nullptr) { m.nrs_heap->Release();     m.nrs_heap = nullptr; }
    if (m.nrs_color    != nullptr) { m.nrs_color->Release();    m.nrs_color = nullptr; }
    if (m.nrs_out      != nullptr) { m.nrs_out->Release();      m.nrs_out = nullptr; }
    for (unsigned pi = 0; pi < stream_state::MAX_PASSES; ++pi)
    {
        if (m.b1_color[pi] != nullptr) { m.b1_color[pi]->Release(); m.b1_color[pi] = nullptr; }
        if (m.b1_out[pi] != nullptr) { m.b1_out[pi]->Release(); m.b1_out[pi] = nullptr; }
    }
    if (m.nrs_native   != nullptr) { m.nrs_native->Release();   m.nrs_native = nullptr; }
    if (m.nev != nullptr) { CloseHandle(m.nev); m.nev = nullptr; }
    if (m.ninput != nullptr) { m.ninput->Release(); m.ninput = nullptr; }
    if (m.nf != nullptr) { m.nf->Release(); m.nf = nullptr; }
    if (m.nl != nullptr) { m.nl->Release(); m.nl = nullptr; }
    if (m.na != nullptr) { m.na->Release(); m.na = nullptr; }
    if (m.nq != nullptr) { m.nq->Release(); m.nq = nullptr; }
    m.nrs_dev = nullptr;
    m.ngx_dev = nullptr;
    mgpu::diag::info("[MGPU][B1] GPU0 local NR release end");
}

static bool local_nr_arm(stream_state &s)
{
    stream_state &m = main_nr_str();
    if (m.nr_ok) return true;
    if (m.nr_tried || main_nr_faulted || s.gdev == nullptr) return false;
    m.nr_tried = true;
    m.width = s.width;
    m.height = s.height;
    m.format = s.format;
    // Features and scratch are allocated once at the native cap. Each evaluate
    // uses its own logical subrect, so a slider never rebuilds an NGX feature.
    m.nrs_w = s.local_nr_max_width ? s.local_nr_max_width : s.width;
    m.nrs_h = s.local_nr_max_height ? s.local_nr_max_height : s.height;
    m.nrs_cap_w = m.nrs_w;
    m.nrs_cap_h = m.nrs_h;
    m.nrs_sw = m.nrs_w;
    m.nrs_sh = m.nrs_h;
    m.nr_built = 3u;
    m.passes = s.passes;
    m.nr_reset_mask = (1u << m.nr_built) - 1u;
    for (unsigned pi = 0; pi < m.nr_built; ++pi)
    {
        m.nrs_pw[pi] = s.nrs_pw[pi];
        m.nrs_ph[pi] = s.nrs_ph[pi];
        m.nrs_pscale[pi] = s.nrs_pscale[pi];
        m.nr_preset_pass[pi] = s.nr_preset_pass[pi];
        m.nr_preset_built[pi] = r361_eff_preset(s, pi, s.passes);
        s.nr_preset_built[pi] = m.nr_preset_built[pi];
    }
    m.nr_preset_any = s.nr_preset_any;
    m.nr_preset1_x2 = s.nr_preset1_x2;
    m.nrs_scale = s.nrs_scale;
    m.nrs_gain = ini_read_float("NRDetailGain", 1.0f);
    m.nrs_mode = (unsigned)ini_read_float("NRMode", 1.0f);
    m.nrs_sharpness = ini_read_float("NRSharpness", 0.50f);
    m.nrs_color_strength = ini_read_float("NRColorStrength", 0.90f);
    m.nrs_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m.nrs_out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m.nrs_dev = s.gdev;
    m.nrs_reset_next = false;
    m.tuning_on = s.tuning_on;
    m.tone_strength = s.tone_strength;
    m.structure_strength = s.structure_strength;
    m.skin_strength = s.skin_strength;
    m.style = s.style;
    m.auto_mask = s.auto_mask;
    m.face_mask_strength = s.face_mask_strength;
    m.set_n = s.set_n;
    for (unsigned i = 0; i < s.set_n; ++i)
    {
        memcpy(m.set_key[i], s.set_key[i], sizeof m.set_key[i]);
        m.set_f[i] = s.set_f[i];
        m.set_u[i] = s.set_u[i];
        m.set_is_float[i] = s.set_is_float[i];
    }

    char line[900];
    snprintf(line, sizeof line,
             "[MGPU][FACE-MASK][B1] FaceMaskStrength=%.2f; every pass sets UseAutoMask=1. "
             "Zero keeps full-frame neural output; character guard is independent.",
             m.face_mask_strength);
    mgpu::diag::info(line);
    HRESULT hr = S_OK;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s.gdev->CreateCommandQueue(&qd, IID_PPV_ARGS(&m.nq));
    if (SUCCEEDED(hr)) hr = s.gdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m.na));
    if (SUCCEEDED(hr)) hr = s.gdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m.na, nullptr, IID_PPV_ARGS(&m.nl));
    if (SUCCEEDED(hr)) hr = m.nl->Close();
    if (SUCCEEDED(hr)) hr = s.gdev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m.nf));
    if (SUCCEEDED(hr)) hr = s.gdev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m.ninput));
    if (SUCCEEDED(hr))
    {
        m.nev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m.nev == nullptr) hr = E_FAIL;
    }
    if (SUCCEEDED(hr))
        hr = make_tex(s.gdev, s.width, s.height, s.format, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, &m.nrs_native);
    if (FAILED(hr) || !nrs_init(s.gdev, m, line, sizeof line) || !nrs_alloc(s.gdev, m, line, sizeof line))
    {
        snprintf(line, sizeof line, "[MGPU][B1] GPU0 queue/scratch init failed hr=0x%08X; current-native fallback", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }
    for (unsigned pi = 0; pi < m.nr_built && SUCCEEDED(hr); ++pi)
    {
        hr = make_tex(s.gdev, m.nrs_pw[pi], m.nrs_ph[pi], s.format,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &m.b1_color[pi]);
        if (SUCCEEDED(hr))
            hr = make_tex(s.gdev, m.nrs_pw[pi], m.nrs_ph[pi], s.format,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &m.b1_out[pi]);
    }
    if (FAILED(hr))
    {
        mgpu::diag::error("[MGPU][B1] exact per-pass texture allocation failed; current-native fallback");
        return false;
    }

    if (ini_read_flag("B1StageTimes"))
    {
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = 6u;
        HRESULT thr = s.gdev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&m.b1_ts_heap));
        if (SUCCEEDED(thr)) thr = make_buf(s.gdev, 6u * 8u, D3D12_HEAP_TYPE_READBACK, &m.b1_ts_read);
        if (SUCCEEDED(thr)) thr = m.nq->GetTimestampFrequency(&m.b1_ts_freq);
        if (FAILED(thr) || m.b1_ts_freq == 0ull)
        {
            if (m.b1_ts_read != nullptr) { m.b1_ts_read->Release(); m.b1_ts_read = nullptr; }
            if (m.b1_ts_heap != nullptr) { m.b1_ts_heap->Release(); m.b1_ts_heap = nullptr; }
            mgpu::diag::warn("[MGPU][B1] GPU0 stage timestamps unavailable; rendering continues without them");
        }
        else mgpu::diag::info("[MGPU][B1] B1StageTimes=1: native-copy-in, each NR+resize and answer-copy-out use GPU0 timestamps");
    }

    ngx_modules mods{};
    mods.core = ngx_find_core(nullptr);
    char snippet_where[MAX_PATH * 2] = {};
    mods.snippet = nr_load_private_snippet(snippet_where, sizeof snippet_where);
    if (mods.snippet == nullptr)
    {
        mods.snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
        if (mods.snippet == nullptr) mods.snippet = LoadLibraryW(L"nvngx_dlssnr.dll");
    }
    if (mods.core == nullptr || mods.snippet == nullptr)
    {
        mgpu::diag::error("[MGPU][B1] NGX core/private NR snippet unavailable; current-native fallback");
        return false;
    }

    char w[9][160] = {};
    char wi[160] = {};
    ngx_pf_init p_init = (ngx_pf_init)ngx_resolve(mods, "NVSDK_NGX_D3D12_Init", ngx_prefer::core, w[0], sizeof w[0]);
    ngx_pf_init_ext p_init_ext = (ngx_pf_init_ext)ngx_resolve(mods, "NVSDK_NGX_D3D12_Init_Ext", ngx_prefer::core, wi, sizeof wi);
    ngx_pf_get_cap_params p_caps = (ngx_pf_get_cap_params)ngx_resolve(mods, "NVSDK_NGX_D3D12_GetCapabilityParameters", ngx_prefer::core, w[1], sizeof w[1]);
    ngx_pf_init_ext p_snip_init = (ngx_pf_init_ext)ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_Init_Ext", w[2], sizeof w[2]);
    ngx_pf_populate_params p_pop = (ngx_pf_populate_params)ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl", w[3], sizeof w[3]);
    ngx_pf_create_feature p_create = (ngx_pf_create_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_CreateFeature", ngx_prefer::snippet, w[4], sizeof w[4]);
    m.nr_eval = (ngx_pf_evaluate_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_EvaluateFeature", ngx_prefer::snippet, w[5], sizeof w[5]);
    m.nr_release = (ngx_pf_release_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_ReleaseFeature", ngx_prefer::snippet, w[6], sizeof w[6]);
    m.ngx_destroy_params = (ngx_pf_destroy_params)ngx_resolve(mods, "NVSDK_NGX_D3D12_DestroyParameters", ngx_prefer::core, w[7], sizeof w[7]);
    m.nr_cre_v = (void *)p_create;
    m.ngx_dev = s.gdev;
    if (p_caps == nullptr || p_snip_init == nullptr || p_pop == nullptr || p_create == nullptr ||
        m.nr_eval == nullptr || m.nr_release == nullptr)
    {
        mgpu::diag::error("[MGPU][B1] NGX entry point resolution failed; current-native fallback");
        return false;
    }

    wchar_t data_path[MAX_PATH] = {};
    wchar_t mp[MAX_PATH] = {};
    const DWORD mn = GetModuleFileNameW(mgpu::module_handle(), mp, MAX_PATH);
    if (mn != 0 && mn < MAX_PATH)
    {
        size_t cut = 0;
        for (size_t i = 0; i + 1 < (size_t)mn; ++i) if (mp[i] == L'\\') cut = i + 1;
        for (size_t i = 0; i < cut; ++i) data_path[i] = mp[i];
    }

    NVSDK_NGX_FeatureCommonInfo common{};
    ngx_logging_info(common.LoggingInfo, "B1-LOCAL-NR");
    char which[300] = {};
    bool callback = false;
    NVSDK_NGX_Result r = ngx_core_init_ladder(p_init_ext, p_init, ngx_app_id(), data_path,
                                               s.gdev, &common, which, sizeof which, &callback);
    snprintf(line, sizeof line, "[MGPU][B1] GPU0 %s result=0x%08X (%s), private snippet=%s",
             which, (unsigned)r, ngx_result_name(r), snippet_where[0] ? snippet_where : "no");
    mgpu::diag::info(line);
    r = p_caps(&m.nr_params);
    if (r != NVSDK_NGX_Result_Success || m.nr_params == nullptr)
    {
        mgpu::diag::error("[MGPU][B1] GetCapabilityParameters failed; current-native fallback");
        return false;
    }
    (void)p_snip_init(ngx_app_id(), data_path, s.gdev, NVSDK_NGX_Version_API, m.nr_params);
    (void)p_pop(m.nr_params);

    m.nr_params->Set("DLSSNR.Color", m.nrs_color);
    m.nr_params->Set("DLSSNR.Output", m.nrs_out);
    m.nr_params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)m.nrs_cap_w);
    m.nr_params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)m.nrs_cap_h);
    m.nr_params->Set("DLSSNR.Width", (unsigned int)m.nrs_cap_w);
    m.nr_params->Set("DLSSNR.Height", (unsigned int)m.nrs_cap_h);
    m.nr_params->Set("DLSSNR.ColorSubrectBaseX", 0u);
    m.nr_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    m.nr_params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)m.nrs_cap_w);
    m.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)m.nrs_cap_h);
    m.nr_params->Set("DLSSNR.OutputSubrectBaseX", 0u);
    m.nr_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    m.nr_params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)m.nrs_cap_w);
    m.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)m.nrs_cap_h);
    if (m.set_n > 0u && ini_read_flag("NRSetAtCreate"))
        for (unsigned i = 0; i < m.set_n; ++i)
        {
            if (m.set_is_float[i]) m.nr_params->Set(m.set_key[i], m.set_f[i]);
            else m.nr_params->Set(m.set_key[i], m.set_u[i]);
        }

    HRESULT ch = m.na->Reset();
    if (SUCCEEDED(ch)) ch = m.nl->Reset(m.na, nullptr);
    if (FAILED(ch)) return false;
    arm_sentinel_set();
    bool all_created = true;
    for (unsigned pi = 0; pi < m.nr_built; ++pi)
    {
        const int preset = r361_eff_preset(s, pi, s.passes);
        m.nr_preset_built[pi] = preset;
        s.nr_preset_built[pi] = preset;
        if (m.nr_preset_any)
            m.nr_params->Set("DLSSNR.Hint.Render.Preset", (unsigned int)preset);
        m.nr_params->Set("DLSSNR.Color", m.b1_color[pi]);
        m.nr_params->Set("DLSSNR.Output", m.b1_out[pi]);
        m.nr_params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)m.nrs_pw[pi]);
        m.nr_params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)m.nrs_ph[pi]);
        m.nr_params->Set("DLSSNR.Width", (unsigned int)m.nrs_pw[pi]);
        m.nr_params->Set("DLSSNR.Height", (unsigned int)m.nrs_ph[pi]);
        m.nr_params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)m.nrs_pw[pi]);
        m.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)m.nrs_ph[pi]);
        m.nr_params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)m.nrs_pw[pi]);
        m.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)m.nrs_ph[pi]);
        unsigned long seh = 0;
        r = ngx_create_guarded((ngx_pf_create_seh_fn)p_create, m.nl,
                               (NVSDK_NGX_Feature)NVSDK_NGX_Feature_Reserved18,
                               m.nr_params, &m.nr_handle[pi], &seh);
        if ((unsigned)r == NGX_RESULT_MGPU_SEH_FAULT)
        {
            main_nr_faulted = true;
            arm_fault_recover(s, seh);
            mgpu::diag::error("[MGPU][B1] GPU0 CreateFeature faulted; damaged init list was NOT executed");
            return false;
        }
        snprintf(line, sizeof line,
                 "[MGPU][B1] GPU0 CreateFeature P%u cap=%ux%u preset=%d result=0x%08X (%s) handle=%p; cadence=1, Depth/MVec unbound",
                 pi + 1u, m.nrs_pw[pi], m.nrs_ph[pi], preset,
                 (unsigned)r, ngx_result_name(r), (void *)m.nr_handle[pi]);
        mgpu::diag::info(line);
        if (r != NVSDK_NGX_Result_Success || m.nr_handle[pi] == nullptr) all_created = false;
    }
    if (m.nr_preset_any) m.nr_params->Set("DLSSNR.Hint.Render.Preset", 0u);
    arm_sentinel_clear();
    ch = m.nl->Close();
    if (SUCCEEDED(ch))
    {
        ID3D12CommandList *const lists[1] = { m.nl };
        m.nq->ExecuteCommandLists(1, lists);
        ++m.nf_value;
        ch = m.nq->Signal(m.nf, m.nf_value);
        if (SUCCEEDED(ch))
        {
            (void)m.nf->SetEventOnCompletion(m.nf_value, m.nev);
            if (WaitForSingleObject(m.nev, 20000) != WAIT_OBJECT_0) ch = E_FAIL;
        }
    }
    m.nr_ok = SUCCEEDED(ch) && all_created;
    if (m.nr_ok)
        mgpu::diag::info("[MGPU][B1] ARMED: GPU0 owns three independent NR handles; Passes=1..3 evaluate EVERY frame and only the final answer crosses to GPU1");
    else
        mgpu::diag::error("[MGPU][B1] GPU0 three-pass NR did not arm; every frame uses current-native fallback");
    return m.nr_ok;
}

static bool local_nr_submit(stream_state &s, stream_state &m, unsigned long long fi)
{
    HRESULT hr = m.nl->Close();
    if (SUCCEEDED(hr)) hr = m.nq->Wait(m.ninput, (UINT64)fi);
    if (SUCCEEDED(hr))
    {
        ID3D12CommandList *const lists[1] = { m.nl };
        m.nq->ExecuteCommandLists(1, lists);
        ++m.nf_value;
        hr = m.nq->Signal(m.nf, m.nf_value);
    }
    if (SUCCEEDED(hr)) hr = m.nq->Signal(s.gfence, (UINT64)fi);
    if (FAILED(hr))
    {
        char line[300];
        snprintf(line, sizeof line, "[MGPU][B1] private submit f=%llu failed hr=0x%08X; game queue publishes native fallback", fi, (unsigned)hr);
        mgpu::diag::error(line);
        m.nr_ok = false;
        return false;
    }
    m.nr_submitted = fi;
    s.gsignaled = fi;
    return true;
}

static bool local_nr_signal_input(stream_state &s, ID3D12CommandQueue *gq)
{
    if (!s.local_nr || gq == nullptr || s.produced == 0) return false;
    stream_state &m = main_nr_str();
    if (m.ninput == nullptr) return false;
    const bool private_owns_final = m.nr_ok && m.nr_submitted == s.produced;
    const HRESULT hr = gq->Signal(m.ninput, (UINT64)s.produced);
    if (FAILED(hr))
    {
        char line[260];
        snprintf(line, sizeof line, "[MGPU][B1] ninput Signal f=%llu failed hr=0x%08X", s.produced, (unsigned)hr);
        mgpu::diag::error(line);
    }
    return private_owns_final;
}

static bool local_nr_record(stream_state &s, unsigned slot, UINT64 slot_off,
                            unsigned long long fi)
{
    stream_state &m = main_nr_str();
    if (!mgpu::b1::nr_due(fi, s.nr_cadence)) return false;
    if (s.fault_id == stream_state::FK_NR_FIRST && fi == 1ull)
    {
        mgpu::diag::warn("[MGPU][B1][FAULT nr_first] f=1 has no NR answer; current-native fallback");
        return false;
    }
    if (!m.nr_ok) return false;
    if (m.nl == nullptr || m.na == nullptr || m.nq == nullptr || m.ninput == nullptr ||
        m.nrs_native == nullptr || m.nrs_color == nullptr || m.nrs_out == nullptr ||
        m.nr_params == nullptr || m.nr_handle[0] == nullptr || s.gxfer == nullptr || s.gfence == nullptr)
        return false;

    // The previous private submission became runnable at this frame's earlier
    // ninput signal. Never reset its allocator until its own fence has passed.
    if (m.nf_value != 0 && m.nf->GetCompletedValue() < m.nf_value)
    {
        HRESULT wh = m.nf->SetEventOnCompletion(m.nf_value, m.nev);
        if (SUCCEEDED(wh) && WaitForSingleObject(m.nev, 2000) != WAIT_OBJECT_0) wh = E_FAIL;
        if (FAILED(wh))
        {
            mgpu::diag::error("[MGPU][B1] private fence wait timed out; current-native fallback");
            m.nr_ok = false;
            return false;
        }
    }

    b1_ts_collect(m); // previous submission is fenced; readback needs no new wait

    // Apply menu edits on GPU0. NGX reset is consumed here; PX1 reset remains
    // queued for GPU1, whose thread owns that history. No cross-thread reset.
    nrs_flush_staged_scales(s);
    m.nr_reset_mask |= s.local_nr_ngx_reset_mask.exchange(0u, std::memory_order_acq_rel);
    // The UI owns s; copy this independent value before every active pass chain.
    m.face_mask_strength = s.face_mask_strength;
    // Configurable split point. B1ProducerPasses=2 keeps P1/P2 on GPU0 and
    // sends only the final low-resolution answer to GPU1 for composition.
    const unsigned requested = (s.passes >= 1u && s.passes <= 3u) ? s.passes : 1u;
    const int producer_cfg = (int)ini_read_float("B1ProducerPasses", 3.0f);
    const unsigned producer_passes = (producer_cfg >= 1 && producer_cfg <= 3) ? (unsigned)producer_cfg : 3u;
    const unsigned active = requested < producer_passes ? requested : producer_passes;
    m.passes = active;
    for (unsigned pi = 0; pi < active; ++pi)
    {
        if (m.nr_handle[pi] == nullptr) return false;
        const bool reduced = (s.nrs_pass_mask & (1u << pi)) != 0u;
        m.nrs_pw[pi] = reduced ? s.nrs_pw[pi] : s.width;
        m.nrs_ph[pi] = reduced ? s.nrs_ph[pi] : s.height;
        m.nrs_pscale[pi] = s.nrs_pscale[pi];
        if (!mgpu::b1::valid_answer_extent(m.nrs_pw[pi], m.nrs_ph[pi],
                                           m.nrs_cap_w, m.nrs_cap_h)) return false;
    }

    HRESULT hr = m.na->Reset();
    if (SUCCEEDED(hr)) hr = m.nl->Reset(m.na, nullptr);
    if (FAILED(hr))
    {
        m.nr_ok = false;
        return false;
    }

    // Network changes are recreated on GPU0 only after the previous private
    // submission retired. This is the same ownership rule as normal evaluates;
    // the UI thread never releases a live NGX handle.
    unsigned recreate = s.nr_recreate_mask;
    if (s.nr_p1_recreate_req) recreate |= 1u;
    s.nr_recreate_mask = 0u;
    s.nr_p1_recreate_req = false;
    for (unsigned pi = 0; pi < active; ++pi)
        if (m.b1_color[pi] == nullptr || m.b1_out[pi] == nullptr ||
            m.b1_color[pi]->GetDesc().Width != m.nrs_pw[pi] ||
            m.b1_color[pi]->GetDesc().Height != m.nrs_ph[pi])
            recreate |= 1u << pi;
    if (recreate != 0u)
    {
        for (unsigned pi = 0; pi < 3u; ++pi)
        {
            if (!(recreate & (1u << pi))) continue;
            const int want = r361_eff_preset(s, pi, active);
            const bool extent_changed = m.b1_color[pi] == nullptr || m.b1_out[pi] == nullptr ||
                m.b1_color[pi]->GetDesc().Width != m.nrs_pw[pi] ||
                m.b1_color[pi]->GetDesc().Height != m.nrs_ph[pi];
            if (extent_changed)
            {
                if (m.b1_color[pi] != nullptr) m.b1_color[pi]->Release();
                if (m.b1_out[pi] != nullptr) m.b1_out[pi]->Release();
                m.b1_color[pi] = nullptr;
                m.b1_out[pi] = nullptr;
                HRESULT tr = make_tex(s.gdev, m.nrs_pw[pi], m.nrs_ph[pi], s.format,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &m.b1_color[pi]);
                if (SUCCEEDED(tr))
                    tr = make_tex(s.gdev, m.nrs_pw[pi], m.nrs_ph[pi], s.format,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &m.b1_out[pi]);
                if (FAILED(tr)) { m.nr_ok = false; return false; }
            }
            m.nr_params->Set("DLSSNR.Color", m.b1_color[pi]);
            m.nr_params->Set("DLSSNR.Output", m.b1_out[pi]);
            m.nr_params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)m.nrs_pw[pi]);
            m.nr_params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)m.nrs_ph[pi]);
            m.nr_params->Set("DLSSNR.Width", (unsigned int)m.nrs_pw[pi]);
            m.nr_params->Set("DLSSNR.Height", (unsigned int)m.nrs_ph[pi]);
            m.nr_params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)m.nrs_pw[pi]);
            m.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)m.nrs_ph[pi]);
            m.nr_params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)m.nrs_pw[pi]);
            m.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)m.nrs_ph[pi]);
            if (!extent_changed && m.nr_handle[pi] != nullptr && want == m.nr_preset_built[pi]) continue;
            if (m.nr_handle[pi] != nullptr)
            {
                (void)m.nr_release(m.nr_handle[pi]);
                m.nr_handle[pi] = nullptr;
            }
            m.nr_params->Set("DLSSNR.Hint.Render.Preset", (unsigned int)want);
            arm_sentinel_set();
            unsigned long create_seh = 0;
            const NVSDK_NGX_Result cr = ngx_create_guarded(
                (ngx_pf_create_seh_fn)m.nr_cre_v, m.nl,
                (NVSDK_NGX_Feature)NVSDK_NGX_Feature_Reserved18,
                m.nr_params, &m.nr_handle[pi], &create_seh);
            arm_sentinel_clear();
            if ((unsigned)cr == NGX_RESULT_MGPU_SEH_FAULT ||
                cr != NVSDK_NGX_Result_Success || m.nr_handle[pi] == nullptr)
            {
                char line[360];
                snprintf(line, sizeof line,
                         "[MGPU][B1] GPU0 live CreateFeature P%u failed 0x%08X SEH=0x%08lX; current-native fallback",
                         pi + 1u, (unsigned)cr, create_seh);
                mgpu::diag::error(line);
                m.nr_ok = false;
                if ((unsigned)cr == NGX_RESULT_MGPU_SEH_FAULT) main_nr_faulted = true;
                return false;
            }
            m.nr_preset_built[pi] = want;
            s.nr_preset_built[pi] = want;
            m.nr_reset_mask |= 1u << pi;
            s.local_nr_px1_reset_mask.fetch_or(1u << pi, std::memory_order_release);
            char line[360];
            snprintf(line, sizeof line,
                     "[MGPU][B1] GPU0 live CreateFeature P%u cap=%ux%u preset=%d handle=%p; retired prior handle first",
                     pi + 1u, m.nrs_cap_w, m.nrs_cap_h, want, (void *)m.nr_handle[pi]);
            mgpu::diag::info(line);
        }
        m.nr_params->Set("DLSSNR.Hint.Render.Preset", 0u);
    }

    if (m.b1_ts_heap != nullptr)
        m.nl->EndQuery(m.b1_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0u);
    D3D12_TEXTURE_COPY_LOCATION ns{}, nd{};
    ns.pResource = s.gxfer;
    ns.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    ns.PlacedFootprint = s.fp;
    ns.PlacedFootprint.Offset = slot_off + SEAL_STRIDE;
    nd.pResource = m.nrs_native;
    nd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    nd.SubresourceIndex = 0;
    m.nl->CopyTextureRegion(&nd, 0, 0, 0, &ns, nullptr);
    barrier(m.nl, m.nrs_native, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (m.b1_ts_heap != nullptr)
        m.nl->EndQuery(m.b1_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1u);

    m.width = s.width;
    m.height = s.height;
    bool ok = true;
    bool faulted = false;
    bool injected_eval_failure = false;
    unsigned failed_pass = 0u;
    unsigned long eval_seh = 0;
    NVSDK_NGX_Result er = NVSDK_NGX_Result_Success;
    unsigned src_w = s.width, src_h = s.height;
    ID3D12Resource *src = m.nrs_native;

    for (unsigned pi = 0; pi < active; ++pi)
    {
        const unsigned pw = m.nrs_pw[pi];
        const unsigned ph = m.nrs_ph[pi];
        const unsigned bit = 1u << pi;
        const unsigned reset = (m.nr_reset_mask & bit) ? 1u : 0u;
        ID3D12Resource *const color = m.b1_color[pi];
        ID3D12Resource *const output = m.b1_out[pi];
        if (color == nullptr || output == nullptr)
        {
            ok = false;
            failed_pass = pi;
            break;
        }

        // Every resource descriptor matches this pass's real extent. There is
        // no native-sized stale region for NGX to read outside a logical subrect.
        if (pi != 0u)
            barrier(m.nl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        nrs_dispatch_down(m, src, pi, src_w, src_h, color);
        if (pi != 0u)
            barrier(m.nl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(m.nl, color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m.nr_params->Set("DLSSNR.Color", color);
        m.nr_params->Set("DLSSNR.Output", output);
        m.nr_params->Set("DLSSNR.Depth", (ID3D12Resource *)nullptr);
        m.nr_params->Set("DLSSNR.MVec", (ID3D12Resource *)nullptr);
        m.nr_params->Set("DLSSNR.Width", (unsigned int)pw);
        m.nr_params->Set("DLSSNR.Height", (unsigned int)ph);
        m.nr_params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)pw);
        m.nr_params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)ph);
        m.nr_params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        m.nr_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        m.nr_params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)pw);
        m.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)ph);
        m.nr_params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        m.nr_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        m.nr_params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)pw);
        m.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)ph);
        m.nr_params->Set("DLSSNR.Intensity", 1.0f);
        if (m.tuning_on)
        {
            m.nr_params->Set("DLSSNR.LocalToneStrength", m.tone_strength);
            m.nr_params->Set("DLSSNR.LocalStructureStrength", m.structure_strength);
            m.nr_params->Set("DLSSNR.SkinStructureStrength", m.skin_strength);
            m.nr_params->Set("DLSSNR.Style", m.style);
            m.nr_params->Set("DLSSNR.UseAutoMask", m.auto_mask ? 1u : 0u);
        }
        for (unsigned i = 0; i < m.set_n; ++i)
        {
            if (m.set_is_float[i]) m.nr_params->Set(m.set_key[i], m.set_f[i]);
            else m.nr_params->Set(m.set_key[i], m.set_u[i]);
        }
        // The dedicated face-mask slider wins over legacy Set.* and Tuning keys.
        // Mask strength zero does NOT bypass NR or composite the original image.
        m.nr_params->Set("DLSSNR.SkinStructureStrength", m.face_mask_strength);
        m.nr_params->Set("DLSSNR.UseAutoMask", 1u);
        m.nr_params->Set("DLSSNR.Reset", reset);

        eval_seh = 0;
        er = ngx_eval_guarded(m.nr_eval, m.nl, m.nr_handle[pi], m.nr_params, &eval_seh);
        faulted = (unsigned)er == NGX_RESULT_MGPU_SEH_FAULT;
        injected_eval_failure = s.fault_id == stream_state::FK_NR_EVAL && fi == 30ull && pi == 0u;
        ok = er == NVSDK_NGX_Result_Success && !injected_eval_failure;
        barrier(m.nl, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!ok)
        {
            failed_pass = pi;
            break;
        }

        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = output;
        m.nl->ResourceBarrier(1, &ub);
        if (m.b1_ts_heap != nullptr)
            m.nl->EndQuery(m.b1_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 2u + pi);
        m.nr_reset_mask &= ~bit;
        ++m.nr_evals;
        ++s.local_nr_evals;
        if (fi <= 2ull || fi % 120ull == 0ull)
        {
            char line[440];
            snprintf(line, sizeof line,
                     "[MGPU][B1] GPU0 f=%llu slot=%u Evaluate P%u #%llu in=%ux%u out=%ux%u reset=%u; exact textures, independent handle=%p",
                     fi, slot, pi + 1u, m.nr_evals, src_w, src_h, pw, ph, reset,
                     (void *)m.nr_handle[pi]);
            mgpu::diag::info(line);
        }
        src = output;
        src_w = pw;
        src_h = ph;
    }

    if (ok)
    {
        // Only the final exact-size answer occupies the ring.
        const unsigned final_pass = active - 1u;
        barrier(m.nl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION cs{}, cd{};
        cs.pResource = src;
        cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.SubresourceIndex = 0;
        cd.pResource = s.gxfer;
        cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        cd.PlacedFootprint = s.local_nr_fp;
        cd.PlacedFootprint.Offset = slot_off + s.local_nr_off;
        cd.PlacedFootprint.Footprint.Width = src_w;
        cd.PlacedFootprint.Footprint.Height = src_h;
        const D3D12_BOX answer_box{0u, 0u, 0u, src_w, src_h, 1u};
        m.nl->CopyTextureRegion(&cd, 0, 0, 0, &cs, &answer_box);
        barrier(m.nl, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.local_nr_width = src_w;
        s.local_nr_height = src_h;
        s.local_nr_answer_pass = final_pass;
    }
    else
    {
        // No part of a broken chain is published. All active histories reset
        // before the next answer so a missed downstream frame cannot linger.
        m.nr_reset_mask |= (1u << active) - 1u;
        ++m.nr_fails;
        ++s.local_nr_failures;
        if (faulted)
        {
            main_nr_faulted = true;
            m.nr_ok = false;
        }
    }
    nrs_to(m, m.nrs_out, &m.nrs_out_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    barrier(m.nl, m.nrs_native, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);

    if (ok && m.b1_ts_heap != nullptr)
    {
        m.nl->EndQuery(m.b1_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 2u + active);
        m.b1_ts_marks = active + 3u;
        m.nl->ResolveQueryData(m.b1_ts_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                               0u, m.b1_ts_marks, m.b1_ts_read, 0u);
        m.b1_ts_pending = true;
    }
    if (!ok)
    {
        m.b1_ts_pending = false;
        char line[560];
        snprintf(line, sizeof line,
                 "[MGPU][B1]%s GPU0 f=%llu slot=%u Evaluate P%u failed 0x%08X (%s) SEH=0x%08lX; whole-chain current-native fallback%s",
                 injected_eval_failure ? "[FAULT nr_eval]" : "",
                 fi, slot, failed_pass + 1u, (unsigned)er, ngx_result_name(er), eval_seh,
                 faulted ? "; damaged list NOT executed" : "");
        mgpu::diag::error(line);
    }
    if (faulted) return false;
    const bool submitted = local_nr_submit(s, m, fi);
    return ok && submitted;
}

#include "local_gpu1_nr.inc"

static bool stream_composite_create(stream_state &s, ID3D12Device *ndev)
{
    char line[700];
    s.tuning_on = ini_read_flag("Tuning");
    s.tone_strength = ini_read_float("ToneStrength", 1.0f);
    s.structure_strength = ini_read_float("StructureStrength", 1.0f);
    s.skin_strength = ini_read_float("SkinStrength", 1.0f);
    s.style = ini_read_float("Style", 0.0f);
    s.auto_mask = ini_read_flag("AutoMask");
    s.face_mask_strength = std::clamp(ini_read_float("FaceMaskStrength", 0.30f), 0.0f, 1.0f);
    s.nrs_on = true;
    s.nrs_dev = ndev;
    // Consumer scratch and every PX1 low-capacity allocation use the native
    // maximum. The seal supplies the actual final extent for each frame.
    s.nrs_w = s.local_nr_max_width ? s.local_nr_max_width : s.width;
    s.nrs_h = s.local_nr_max_height ? s.local_nr_max_height : s.height;
    s.nrs_sw = s.nrs_w;
    s.nrs_sh = s.nrs_h;
    s.nrs_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    s.nrs_out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    s.tex_in_rest = D3D12_RESOURCE_STATE_COPY_DEST;

    const DXGI_FORMAT fmt = s.format;
    HRESULT hr = make_tex(ndev, s.width, s.height, fmt, D3D12_RESOURCE_FLAG_NONE,
                          s.tex_in_rest, &s.tex_in[0]);
    if (SUCCEEDED(hr)) hr = make_tex(ndev, s.width, s.height, fmt, D3D12_RESOURCE_FLAG_NONE,
                                     s.tex_in_rest, &s.tex_in[1]);
    if (SUCCEEDED(hr)) s.tex_in_show = s.tex_in[0];
    if (SUCCEEDED(hr)) hr = make_tex(ndev, s.width, s.height, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.tex_out);
    if (SUCCEEDED(hr)) hr = make_tex(ndev, s.width, s.height, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.tex_pong);
    if (SUCCEEDED(hr)) hr = make_tex(ndev, s.nrs_w, s.nrs_h, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.nrs_color);
    if (SUCCEEDED(hr)) hr = make_tex(ndev, s.nrs_w, s.nrs_h, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.nrs_out);
    if (SUCCEEDED(hr)) hr = make_buf(ndev, 1024 + 4 * 32 * 256, D3D12_HEAP_TYPE_READBACK, &s.nr_read);
    if (SUCCEEDED(hr)) hr = make_tex(ndev, s.width, s.height, DXGI_FORMAT_R32G32_FLOAT,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_mvec);
    if (SUCCEEDED(hr))
    {
        const D3D12_RESOURCE_DESC md = s.tex_mvec->GetDesc();
        UINT rows = 0; UINT64 row_bytes = 0;
        ndev->GetCopyableFootprints(&md, 0, 1, 0, &s.mvec_fp, &rows, &row_bytes, &s.mvec_bytes);
        hr = make_buf(ndev, s.mvec_bytes, D3D12_HEAP_TYPE_UPLOAD, &s.mvec_up);
        if (SUCCEEDED(hr))
        {
            D3D12_RANGE none{0, 0};
            hr = s.mvec_up->Map(0, &none, reinterpret_cast<void **>(&s.mvec_cpu));
        }
    }
    if (SUCCEEDED(hr) && s.depth_mode != 0 && s.depth_w != 0 && s.depth_h != 0)
        for (unsigned i = 0; i < 2 && SUCCEEDED(hr); ++i)
            hr = make_tex(ndev, s.depth_w, s.depth_h, (DXGI_FORMAT)s.depth_format,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_depth[i]);
    if (SUCCEEDED(hr) && s.mvec_mode == 3 && s.mvec_w != 0 && s.mvec_h != 0)
    {
        for (unsigned i = 0; i < 2 && SUCCEEDED(hr); ++i)
            hr = make_tex(ndev, s.mvec_w, s.mvec_h, (DXGI_FORMAT)s.mvec_format,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_mvec_r[i]);
        if (SUCCEEDED(hr)) hr = make_tex(ndev, s.mvec_w, s.mvec_h, (DXGI_FORMAT)s.mvec_format,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         &s.tex_mvec_keep);
    }
    if (FAILED(hr))
    {
        snprintf(line, sizeof line, "[MGPU][B1] GPU1 composite resources failed hr=0x%08X", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    const float px1_guard = ini_read_float("PrismX1Guard",
        ini_read_float("PrismSyncGuard", ini_read_float("FinishGuard", 1.0f)));
    for (unsigned pi = 0; pi < 3u; ++pi)
    {
        if (!mgpu::prism_x1m::arm(pi, ndev, s.width, s.height, fmt, s.nrs_w, s.nrs_h))
        {
            snprintf(line, sizeof line, "[MGPU][B1] GPU1 PX1 P%u did not arm", pi + 1u);
            mgpu::diag::error(line);
            return false;
        }
        mgpu::prism_x1m::set_intensity(pi, s.intensity[pi]);
        mgpu::prism_x1m::set_guard(pi, px1_guard);
    }
    (void)mgpu::pfinish::arm(ndev, s.width, s.height, fmt, false);

    s.nr_fp.Offset = 0;
    s.nr_fp.Footprint.Format = fmt;
    s.nr_fp.Footprint.Width = 64;
    s.nr_fp.Footprint.Height = 4;
    s.nr_fp.Footprint.Depth = 1;
    s.nr_fp.Footprint.RowPitch = 256;
    s.nr_final = nullptr;
    const int producer_cfg = (int)ini_read_float("B1ProducerPasses", 3.0f);
    if (producer_cfg < s.passes && !secondary_nr_arm(s, ndev))
        mgpu::diag::error("[MGPU][B1-SPLIT] GPU1 downstream NR unavailable; native fallback");
    snprintf(line, sizeof line,
             "[MGPU][B1] composition armed: producerPasses=%d totalPasses=%u, GPU1 final fusion only when producer completes the chain; native=%ux%u",
             producer_cfg, s.passes, s.width, s.height);
    mgpu::diag::info(line);
    return true;
}
