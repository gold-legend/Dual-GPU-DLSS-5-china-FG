// B1 split NR: GPU0 produces P1; GPU1 owns P2/P3 and final PX1 composition.
// Included after local_nr.inc, inside gpu1_context.cpp's anonymous namespace.

static stream_state &secondary_nr_str()
{
    static stream_state g;
    return g;
}
static bool secondary_p1_is_srv = false;

static void secondary_nr_copy_settings(stream_state &dst, const stream_state &src)
{
    dst.width = src.width;
    dst.height = src.height;
    dst.format = src.format;
    dst.passes = src.passes;
    dst.nrs_w = src.local_nr_max_width ? src.local_nr_max_width : src.width;
    dst.nrs_h = src.local_nr_max_height ? src.local_nr_max_height : src.height;
    dst.nrs_cap_w = dst.nrs_w;
    dst.nrs_cap_h = dst.nrs_h;
    dst.nrs_sw = dst.nrs_w;
    dst.nrs_sh = dst.nrs_h;
    dst.nrs_scale = src.nrs_scale;
    dst.nrs_pass_mask = src.nrs_pass_mask;
    dst.nr_built = 3u;
    dst.nr_preset_any = src.nr_preset_any;
    dst.nr_preset1_x2 = src.nr_preset1_x2;
    for (unsigned pi = 0; pi < 3u; ++pi)
    {
        dst.nrs_pw[pi] = src.nrs_pw[pi];
        dst.nrs_ph[pi] = src.nrs_ph[pi];
        dst.nrs_pscale[pi] = src.nrs_pscale[pi];
        dst.nr_preset_pass[pi] = src.nr_preset_pass[pi];
        dst.nr_preset_built[pi] = r361_eff_preset(src, pi, src.passes);
    }
    dst.nrs_gain = ini_read_float("NRDetailGain", 1.0f);
    dst.nrs_mode = (unsigned)ini_read_float("NRMode", 1.0f);
    dst.nrs_sharpness = ini_read_float("NRSharpness", 0.50f);
    dst.nrs_color_strength = ini_read_float("NRColorStrength", 0.90f);
    dst.nrs_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dst.nrs_out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dst.tuning_on = src.tuning_on;
    dst.tone_strength = src.tone_strength;
    dst.structure_strength = src.structure_strength;
    dst.skin_strength = src.skin_strength;
    dst.style = src.style;
    dst.auto_mask = src.auto_mask;
    dst.face_mask_strength = src.face_mask_strength;
    dst.set_n = src.set_n;
    for (unsigned i = 0; i < src.set_n; ++i)
    {
        memcpy(dst.set_key[i], src.set_key[i], sizeof dst.set_key[i]);
        dst.set_f[i] = src.set_f[i];
        dst.set_u[i] = src.set_u[i];
        dst.set_is_float[i] = src.set_is_float[i];
    }
}

static bool secondary_nr_arm(stream_state &s, ID3D12Device *ndev)
{
    stream_state &g = secondary_nr_str();
    if (g.nr_ok) return true;
    if (g.nr_tried || ndev == nullptr) return false;
    g.nr_tried = true;
    secondary_nr_copy_settings(g, s);
    g.nrs_dev = ndev;

    char line[900];
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = ndev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.nq));
    if (SUCCEEDED(hr)) hr = ndev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.na));
    if (SUCCEEDED(hr)) hr = ndev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.na, nullptr, IID_PPV_ARGS(&g.nl));
    if (SUCCEEDED(hr)) hr = g.nl->Close();
    if (SUCCEEDED(hr)) hr = ndev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.nf));
    if (SUCCEEDED(hr))
    {
        g.nev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g.nev == nullptr) hr = E_FAIL;
    }
    if (FAILED(hr) || !nrs_init(ndev, g, line, sizeof line) || !nrs_alloc(ndev, g, line, sizeof line))
    {
        snprintf(line, sizeof line, "[MGPU][B1-SPLIT] GPU1 queue/scratch init failed hr=0x%08X", (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }
    for (unsigned pi = 0; pi < 3u && SUCCEEDED(hr); ++pi)
    {
        hr = make_tex(ndev, g.nrs_pw[pi], g.nrs_ph[pi], g.format,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g.b1_color[pi]);
        if (SUCCEEDED(hr))
            hr = make_tex(ndev, g.nrs_pw[pi], g.nrs_ph[pi], g.format,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g.b1_out[pi]);
    }
    if (FAILED(hr))
    {
        mgpu::diag::error("[MGPU][B1-SPLIT] GPU1 exact per-pass textures failed");
        return false;
    }

    ngx_modules mods{};
    mods.core = ngx_find_core(nullptr);
    char snippet_where[MAX_PATH * 2] = {};
    mods.snippet = nr_load_private_snippet(snippet_where, sizeof snippet_where);
    if (mods.snippet == nullptr) mods.snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (mods.core == nullptr || mods.snippet == nullptr)
    {
        mgpu::diag::error("[MGPU][B1-SPLIT] GPU1 NGX core/snippet unavailable");
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
    g.nr_eval = (ngx_pf_evaluate_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_EvaluateFeature", ngx_prefer::snippet, w[5], sizeof w[5]);
    g.nr_release = (ngx_pf_release_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_ReleaseFeature", ngx_prefer::snippet, w[6], sizeof w[6]);
    g.ngx_destroy_params = (ngx_pf_destroy_params)ngx_resolve(mods, "NVSDK_NGX_D3D12_DestroyParameters", ngx_prefer::core, w[7], sizeof w[7]);
    g.nr_cre_v = (void *)p_create;
    g.ngx_dev = ndev;
    if (p_caps == nullptr || p_snip_init == nullptr || p_pop == nullptr || p_create == nullptr || g.nr_eval == nullptr || g.nr_release == nullptr)
    {
        mgpu::diag::error("[MGPU][B1-SPLIT] GPU1 NGX entry resolution failed");
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
    ngx_logging_info(common.LoggingInfo, "B1-SPLIT-GPU1");
    char which[300] = {};
    bool callback = false;
    NVSDK_NGX_Result r = ngx_core_init_ladder(p_init_ext, p_init, ngx_app_id(), data_path,
                                               ndev, &common, which, sizeof which, &callback);
    snprintf(line, sizeof line, "[MGPU][B1-SPLIT] GPU1 %s result=0x%08X (%s)",
             which, (unsigned)r, ngx_result_name(r));
    mgpu::diag::info(line);
    r = p_caps(&g.nr_params);
    if (r != NVSDK_NGX_Result_Success || g.nr_params == nullptr)
    {
        mgpu::diag::error("[MGPU][B1-SPLIT] GPU1 GetCapabilityParameters failed");
        return false;
    }
    (void)p_snip_init(ngx_app_id(), data_path, ndev, NVSDK_NGX_Version_API, g.nr_params);
    (void)p_pop(g.nr_params);

    hr = g.na->Reset();
    if (SUCCEEDED(hr)) hr = g.nl->Reset(g.na, nullptr);
    if (FAILED(hr)) return false;
    arm_sentinel_set();
    bool all_created = true;
    for (unsigned pi = 1; pi < 3u; ++pi)
    {
        const int preset = r361_eff_preset(s, pi, s.passes);
        g.nr_preset_built[pi] = preset;
        if (g.nr_preset_any) g.nr_params->Set("DLSSNR.Hint.Render.Preset", (unsigned int)preset);
        g.nr_params->Set("DLSSNR.Color", g.b1_color[pi]);
        g.nr_params->Set("DLSSNR.Output", g.b1_out[pi]);
        g.nr_params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)g.nrs_pw[pi]);
        g.nr_params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)g.nrs_ph[pi]);
        g.nr_params->Set("DLSSNR.Width", (unsigned int)g.nrs_pw[pi]);
        g.nr_params->Set("DLSSNR.Height", (unsigned int)g.nrs_ph[pi]);
        g.nr_params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        g.nr_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        g.nr_params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)g.nrs_pw[pi]);
        g.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)g.nrs_ph[pi]);
        g.nr_params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        g.nr_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        g.nr_params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)g.nrs_pw[pi]);
        g.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)g.nrs_ph[pi]);
        unsigned long seh = 0;
        r = ngx_create_guarded((ngx_pf_create_seh_fn)p_create, g.nl,
                               (NVSDK_NGX_Feature)NVSDK_NGX_Feature_Reserved18,
                               g.nr_params, &g.nr_handle[pi], &seh);
        snprintf(line, sizeof line,
                 "[MGPU][B1-SPLIT] GPU1 CreateFeature P%u %ux%u preset=%d result=0x%08X (%s) handle=%p",
                 pi + 1u, g.nrs_pw[pi], g.nrs_ph[pi], preset,
                 (unsigned)r, ngx_result_name(r), (void *)g.nr_handle[pi]);
        mgpu::diag::info(line);
        if ((unsigned)r == NGX_RESULT_MGPU_SEH_FAULT)
        {
            arm_sentinel_clear();
            return false;
        }
        if (r != NVSDK_NGX_Result_Success || g.nr_handle[pi] == nullptr) all_created = false;
    }
    if (g.nr_preset_any) g.nr_params->Set("DLSSNR.Hint.Render.Preset", 0u);
    arm_sentinel_clear();
    hr = g.nl->Close();
    if (SUCCEEDED(hr))
    {
        ID3D12CommandList *const lists[1] = { g.nl };
        g.nq->ExecuteCommandLists(1, lists);
        ++g.nf_value;
        hr = g.nq->Signal(g.nf, g.nf_value);
        if (SUCCEEDED(hr))
        {
            (void)g.nf->SetEventOnCompletion(g.nf_value, g.nev);
            if (WaitForSingleObject(g.nev, 20000) != WAIT_OBJECT_0) hr = E_FAIL;
        }
    }
    g.nr_ok = SUCCEEDED(hr) && all_created;
    g.nr_reset_mask = 0x6u;
    if (g.nr_ok)
        mgpu::diag::info("[MGPU][B1-SPLIT] ARMED: GPU0=P1, GPU1=P2/P3+GuidedFit+PX1; only the low-resolution P1 answer crosses");
    else
        mgpu::diag::error("[MGPU][B1-SPLIT] GPU1 P2/P3 did not arm");
    return g.nr_ok;
}

static bool secondary_nr_wait_consumer(stream_state &s)
{
    if (s.nf == nullptr || s.nf_value == 0 || s.nf->GetCompletedValue() >= s.nf_value) return true;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr) return false;
    const HRESULT hr = s.nf->SetEventOnCompletion(s.nf_value, ev);
    const DWORD wr = (SUCCEEDED(hr)) ? WaitForSingleObject(ev, 5000) : WAIT_FAILED;
    CloseHandle(ev);
    return SUCCEEDED(hr) && wr == WAIT_OBJECT_0;
}

static bool secondary_nr_prepare(stream_state &s, unsigned active)
{
    stream_state &g = secondary_nr_str();
    if (!g.nr_ok) return false;
    bool changed = false;
    for (unsigned pi = 0; pi < active; ++pi)
    {
        const int preset = r361_eff_preset(s, pi, active);
        if (g.b1_color[pi] == nullptr || g.b1_out[pi] == nullptr ||
            g.b1_color[pi]->GetDesc().Width != s.nrs_pw[pi] ||
            g.b1_color[pi]->GetDesc().Height != s.nrs_ph[pi] ||
            (pi != 0u && preset != g.nr_preset_built[pi])) changed = true;
    }
    if (!changed) return true;
    if (!secondary_nr_wait_consumer(s))
    {
        mgpu::diag::error("[MGPU][B1-SPLIT] consumer drain failed during scale change");
        return false;
    }
    for (unsigned pi = 0; pi < active; ++pi)
    {
        const int preset = r361_eff_preset(s, pi, active);
        const bool extent_changed = g.b1_color[pi] == nullptr || g.b1_out[pi] == nullptr ||
            g.b1_color[pi]->GetDesc().Width != s.nrs_pw[pi] ||
            g.b1_color[pi]->GetDesc().Height != s.nrs_ph[pi];
        if (!extent_changed && (pi == 0u || preset == g.nr_preset_built[pi])) continue;
        g.nrs_pw[pi] = s.nrs_pw[pi];
        g.nrs_ph[pi] = s.nrs_ph[pi];
        if (extent_changed)
        {
            if (pi == 0u) secondary_p1_is_srv = false;
            if (g.b1_color[pi]) { g.b1_color[pi]->Release(); g.b1_color[pi] = nullptr; }
            if (g.b1_out[pi]) { g.b1_out[pi]->Release(); g.b1_out[pi] = nullptr; }
            HRESULT hr = make_tex(g.nrs_dev, g.nrs_pw[pi], g.nrs_ph[pi], g.format,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g.b1_color[pi]);
            if (SUCCEEDED(hr)) hr = make_tex(g.nrs_dev, g.nrs_pw[pi], g.nrs_ph[pi], g.format,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g.b1_out[pi]);
            if (FAILED(hr)) return false;
        }
        if (pi != 0u)
        {
            if (g.nr_handle[pi]) { (void)g.nr_release(g.nr_handle[pi]); g.nr_handle[pi] = nullptr; }
            g.nr_params->Set("DLSSNR.Color", g.b1_color[pi]);
            g.nr_params->Set("DLSSNR.Output", g.b1_out[pi]);
            g.nr_params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)g.nrs_pw[pi]);
            g.nr_params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)g.nrs_ph[pi]);
            g.nr_params->Set("DLSSNR.Width", (unsigned int)g.nrs_pw[pi]);
            g.nr_params->Set("DLSSNR.Height", (unsigned int)g.nrs_ph[pi]);
            g.nr_params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)g.nrs_pw[pi]);
            g.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)g.nrs_ph[pi]);
            g.nr_params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)g.nrs_pw[pi]);
            g.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)g.nrs_ph[pi]);
            g.nr_params->Set("DLSSNR.Hint.Render.Preset", (unsigned int)preset);
            unsigned long seh = 0;
            const NVSDK_NGX_Result r = ngx_create_guarded((ngx_pf_create_seh_fn)g.nr_cre_v, s.nl,
                (NVSDK_NGX_Feature)NVSDK_NGX_Feature_Reserved18, g.nr_params, &g.nr_handle[pi], &seh);
            g.nr_params->Set("DLSSNR.Hint.Render.Preset", 0u);
            if (r != NVSDK_NGX_Result_Success || g.nr_handle[pi] == nullptr) return false;
            g.nr_preset_built[pi] = preset;
            g.nr_reset_mask |= 1u << pi;
        }
    }
    return true;
}

static bool secondary_nr_record(stream_state &s, unsigned long long f, UINT64 slot_off,
                                unsigned p1w, unsigned p1h, unsigned active,
                                ID3D12Resource **final_resource, unsigned *final_w,
                                unsigned *final_h, unsigned *final_pass)
{
    stream_state &g = secondary_nr_str();
    *final_resource = nullptr;
    *final_w = *final_h = *final_pass = 0u;
    if (active < 1u || active > 3u || !secondary_nr_prepare(s, active)) return false;
    g.nl = s.nl;
    g.face_mask_strength = s.face_mask_strength;

    // Every final NR resource is returned to UAV immediately after PX1 compose.
    barrier(s.nl, g.b1_out[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION ns{}, nd{};
    ns.pResource = s.nxfer;
    ns.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    ns.PlacedFootprint = s.local_nr_fp;
    ns.PlacedFootprint.Offset = slot_off + s.local_nr_off;
    ns.PlacedFootprint.Footprint.Width = p1w;
    ns.PlacedFootprint.Footprint.Height = p1h;
    nd.pResource = g.b1_out[0];
    nd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    nd.SubresourceIndex = 0;
    const D3D12_BOX p1box{0u, 0u, 0u, p1w, p1h, 1u};
    s.nl->CopyTextureRegion(&nd, 0, 0, 0, &ns, &p1box);
    ++s.local_nr_unpacks;
    if (active == 1u)
    {
        barrier(s.nl, g.b1_out[0], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        *final_resource = g.b1_out[0];
        *final_w = p1w;
        *final_h = p1h;
        *final_pass = 0u;
        secondary_p1_is_srv = true;
        return true;
    }

    ID3D12Resource *src = g.b1_out[0];
    unsigned src_w = p1w, src_h = p1h;
    barrier(s.nl, src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    secondary_p1_is_srv = false;
    for (unsigned pi = 1; pi < active; ++pi)
    {
        if (g.nr_handle[pi] == nullptr) return false;
        const unsigned pw = g.nrs_pw[pi], ph = g.nrs_ph[pi];
        ID3D12Resource *color = g.b1_color[pi];
        ID3D12Resource *output = g.b1_out[pi];
        barrier(s.nl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        nrs_dispatch_down(g, src, pi, src_w, src_h, color);
        barrier(s.nl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(s.nl, color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.nr_params->Set("DLSSNR.Color", color);
        g.nr_params->Set("DLSSNR.Output", output);
        g.nr_params->Set("DLSSNR.Depth", (ID3D12Resource *)nullptr);
        g.nr_params->Set("DLSSNR.MVec", (ID3D12Resource *)nullptr);
        g.nr_params->Set("DLSSNR.Width", pw);
        g.nr_params->Set("DLSSNR.Height", ph);
        g.nr_params->Set(NVSDK_NGX_Parameter_Width, pw);
        g.nr_params->Set(NVSDK_NGX_Parameter_Height, ph);
        g.nr_params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        g.nr_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        g.nr_params->Set("DLSSNR.ColorSubrectWidth", pw);
        g.nr_params->Set("DLSSNR.ColorSubrectHeight", ph);
        g.nr_params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        g.nr_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        g.nr_params->Set("DLSSNR.OutputSubrectWidth", pw);
        g.nr_params->Set("DLSSNR.OutputSubrectHeight", ph);
        g.nr_params->Set("DLSSNR.Intensity", 1.0f);
        if (g.tuning_on)
        {
            g.nr_params->Set("DLSSNR.LocalToneStrength", g.tone_strength);
            g.nr_params->Set("DLSSNR.LocalStructureStrength", g.structure_strength);
            g.nr_params->Set("DLSSNR.SkinStructureStrength", g.skin_strength);
            g.nr_params->Set("DLSSNR.Style", g.style);
            g.nr_params->Set("DLSSNR.UseAutoMask", g.auto_mask ? 1u : 0u);
        }
        for (unsigned i = 0; i < g.set_n; ++i)
        {
            if (g.set_is_float[i]) g.nr_params->Set(g.set_key[i], g.set_f[i]);
            else g.nr_params->Set(g.set_key[i], g.set_u[i]);
        }
        g.nr_params->Set("DLSSNR.SkinStructureStrength", g.face_mask_strength);
        g.nr_params->Set("DLSSNR.UseAutoMask", 1u);
        g.nr_params->Set("DLSSNR.Reset", (g.nr_reset_mask & (1u << pi)) ? 1u : 0u);
        unsigned long eval_seh = 0;
        const NVSDK_NGX_Result er = ngx_eval_guarded(g.nr_eval, s.nl, g.nr_handle[pi], g.nr_params, &eval_seh);
        barrier(s.nl, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (er != NVSDK_NGX_Result_Success)
        {
            char line[360];
            snprintf(line, sizeof line, "[MGPU][B1-SPLIT] GPU1 Evaluate P%u failed 0x%08X (%s) SEH=0x%08lX",
                     pi + 1u, (unsigned)er, ngx_result_name(er), eval_seh);
            mgpu::diag::error(line);
            g.nr_reset_mask |= 0x6u;
            return false;
        }
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = output;
        s.nl->ResourceBarrier(1, &ub);
        g.nr_reset_mask &= ~(1u << pi);
        ++g.nr_evals;
        if (f <= 2ull || f % 120ull == 0ull)
        {
            char line[360];
            snprintf(line, sizeof line, "[MGPU][B1-SPLIT] GPU1 f=%llu Evaluate P%u in=%ux%u out=%ux%u handle=%p",
                     f, pi + 1u, src_w, src_h, pw, ph, (void *)g.nr_handle[pi]);
            mgpu::diag::info(line);
        }
        src = output;
        src_w = pw;
        src_h = ph;
    }
    barrier(s.nl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    *final_resource = src;
    *final_w = src_w;
    *final_h = src_h;
    *final_pass = active - 1u;
    return true;
}

static void secondary_nr_release()
{
    stream_state &g = secondary_nr_str();
    if (g.nr_release)
        for (int pi = 2; pi >= 1; --pi)
            if (g.nr_handle[pi]) { (void)g.nr_release(g.nr_handle[pi]); g.nr_handle[pi] = nullptr; }
    if (g.ngx_destroy_params && g.nr_params) { (void)g.ngx_destroy_params(g.nr_params); g.nr_params = nullptr; }
    for (unsigned pi = 0; pi < 3u; ++pi)
    {
        if (g.b1_color[pi]) { g.b1_color[pi]->Release(); g.b1_color[pi] = nullptr; }
        if (g.b1_out[pi]) { g.b1_out[pi]->Release(); g.b1_out[pi] = nullptr; }
    }
    if (g.nrs_color) { g.nrs_color->Release(); g.nrs_color = nullptr; }
    if (g.nrs_out) { g.nrs_out->Release(); g.nrs_out = nullptr; }
    if (g.nrs_heap) { g.nrs_heap->Release(); g.nrs_heap = nullptr; }
    if (g.nrs_pso_down) { g.nrs_pso_down->Release(); g.nrs_pso_down = nullptr; }
    if (g.nrs_pso_res) { g.nrs_pso_res->Release(); g.nrs_pso_res = nullptr; }
    if (g.nrs_rs_down) { g.nrs_rs_down->Release(); g.nrs_rs_down = nullptr; }
    if (g.nrs_rs_res) { g.nrs_rs_res->Release(); g.nrs_rs_res = nullptr; }
    if (g.nl) { g.nl->Release(); g.nl = nullptr; }
    if (g.na) { g.na->Release(); g.na = nullptr; }
    if (g.nf) { g.nf->Release(); g.nf = nullptr; }
    if (g.nq) { g.nq->Release(); g.nq = nullptr; }
    if (g.nev) { CloseHandle(g.nev); g.nev = nullptr; }
    g.nr_ok = false;
}
