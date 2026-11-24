// The replay position and its lookaheads: occlusion needs, the precompile prediction, pass continuation, slice-merged and face-split plans, readable-tile specializations, DM_NVN_MAGSCAN.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- Replay position: the lookaheads (cs_hoist_try, lc, mdx_prescan, ixc / wide scans) walk on from here with op_next ----------
static const uint8_t *g_exec_next;   // The op following the current draw in the replay
static const uint8_t *g_exec_cur;    // The current draw's own op (set with g_exec_next)
static const uint8_t *g_ret[16]; static int g_ret_n;   // Call return stack of the replay (at OP_CALL the caller's next op is pushed)
static const uint64_t *g_rest; static int g_nrest;      // Handles in this submission still to be replayed after the current handle
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = h; g_nrest = n; }
typedef struct { const uint8_t *p, *ret[8]; int ret_n, rest_i, bad, pass_multi; } op_cursor;   // pass_multi: a scan that moves no work steps over indirect multi-draws
static int op_next(op_cursor *c, const uint8_t **out, op_hdr *oh, const void **oa) {
    for (int guard = 0; guard < 1000000; guard++) {
        if (!c->p) return 0;
        op_hdr h; memcpy(&h, c->p, sizeof h); const void *a = c->p + 8;
        switch (h.op) {
            // The live draws and their argument addresses are resolved at replay. A speculative scan cannot move work across this range.
            case OP_MULTI_DRAW: if (c->pass_multi) break; c->bad = 1; return 0;
            case OP_END:
                if (c->ret_n) c->p = c->ret[--c->ret_n];
                else if (c->rest_i < g_nrest) c->p = (const uint8_t *)g_rest[c->rest_i++];
                else { c->p = NULL; return 0; }
                continue;
            case OP_JUMP: memcpy(&c->p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (c->ret_n >= 8) { c->bad = 1; return 0; }
                c->ret[c->ret_n++] = c->p + h.size; c->p = (const uint8_t *)t; continue; }
            default: break;
        }
        const uint8_t *p = c->p; c->p += h.size;
        if (out) *out = p; if (oh) *oh = h; if (oa) *oa = a; return 1;
    }
    c->bad = 1; return 0;
}
// DM_NVN_VIS_NEED=0|1|ab (on by default): a draw counts samples for occlusion queries only while a report can see them. A reset drops the slots since the last report unread
// (Occlusion queries), so after a samples-passed reset or report whose next samples-passed op in this submission is a reset, the draws up to it count into slots nothing reads:
// they are encoded with counting off and take no slot (draw_). The reported values are the same. When the next op is a report, or the scan does not find one (the submission's
// end, which adds its last slots at completion; 20,000 ops; a call stack deeper than the cursor's), they count as before. Ocarina of Time puts its own reset and report around
// each queried draw (about 1,800 a frame), and every other draw in between counted: on an M1 at Kokiri Forest about -0.2 ms of GPU time a frame (PR #2574)
static int vis_need_on(void) {
    static int m = -1;
    if (m < 0) { m = sw_mode("DM_NVN_VIS_NEED", 1); if (m != 1) MLOG("DM_NVN_VIS_NEED=%s: every draw counts for occlusion queries once counting began%s", getenv("DM_NVN_VIS_NEED"), m ? ", in the A/B phases it is off in" : ""); }
    return m && sw_on(m);
}
static void vis_need_scan(const uint8_t *next) {   // at a samples-passed reset or report: next, the op after it in replay order
    g_vis_need = 1;
    if (!vis_need_on() || !next || g_ret_n > 8) return;
    op_cursor cur = {.p = next, .ret_n = g_ret_n, .pass_multi = 1}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
    const uint8_t *q; op_hdr h; const void *a;
    for (int guard = 0; guard < 20000 && op_next(&cur, &q, &h, &a); guard++) {
        if (h.op != OP_RESET_COUNTER && h.op != OP_REPORT_COUNTER) continue;
        uint64_t t; memcpy(&t, a, 8); if (t != NVN_COUNTER_TYPE_SAMPLES_PASSED) continue;
        g_vis_need = h.op == OP_REPORT_COUNTER; return;
    }
}
static void draw(const p_draw *dw, const p_draw_idx *di);
static void sd_need(sd_att *t, int why) { if (!t->state) { t->state = 1; t->why = (uint8_t)why; } }   // why: 1 a draw reads it as a texture 2 as an attachment, drawn before cleared 3 copy 4 texture clear 5 scanned to the end undecided 6 no start point / index unavailable
static void sd_finish(void) {
    if (!g_nsd) return;
    // Unchanged, loaded attachments already have their content in memory. Their proof depends only on writes in this pass, independently of future bindless readers or the optional lookahead.
    for (int i = 0; i < g_nsd; i++) sd_need(&g_sd[i], 6);   // Pass ended outside the list / index unavailable: always write back
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i]; int udc_dead = udc_on() && t->load && !t->wrote;
        // A loaded color tile with no raster output or clear holds only the old memory contents. Storing it can overwrite image writes to the same texture
        // (Dark Souls' image-only AO pass). This is required for correctness even when store optimizations are disabled; mixed tile/image writers retain their store.
        int img_dead = t->depth == 0 && t->load && t->img && !t->rast;
        int dead = udc_dead || img_dead; MTLStoreAction sa = dead ? MTLStoreActionDontCare : MTLStoreActionStore;
        if (t->depth == 1) [g_enc setDepthStoreAction:sa]; else if (t->depth == 2) [g_enc setStencilStoreAction:sa]; else [g_enc setColorStoreAction:sa atIndex:(NSUInteger)t->idx];
        g_sd_n_all++; if (dead) { g_sd_n_dead++; g_sd_dead_bytes += t->bytes; g_bw_store -= t->bytes; if (udc_dead) g_udc_saved_bytes += t->bytes; }
        if (!t->state) t->why = 5;
        PD("Z %llu %s ser=%llu lv%u sl%u %s why=%d by=%s slot=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, dead ? "dontcare" : "store", dead ? 0 : t->why, t->why == 1 && t->wkey ? t->wkey : "-", t->wslot);
        if (udc_on()) PD("U %llu %s ser=%llu lv%u sl%u runtime=%s reason=%s state=%d load=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, t->wrote ? "written" : "unwritten", t->wwhy == 1 ? "image_write" : t->wwhy == 2 ? "color_write_mask" : t->wwhy == 4 ? "depth_write" : t->wwhy == 5 ? "stencil_write" : "none", t->state, t->load);
    }
    g_nsd = 0;
}
// Continue scanning in real replay order: current list -> callers at each level (return stack from the top down) -> remaining handles of this submit; the render pass necessarily ends when the submit ends (rp_flush before commit)
// Coverage proof (holds only if all are satisfied; anything unrecognized is treated as not holding):
//   Shader: the vertex position xyz is taken as-is from one vertex attribute, w is 1 or the same attribute's .w (dmsh_reflect raster_facts); the fragment does not discard, does not write sample_mask, and writes this color index
//   State: this attachment has no blending, a full write mask, no alpha-to-coverage, fill mode, no depth test (or ALWAYS), no stencil test, single sample, not 3D
//   Vertices: non-indexed or 8 / 16 / 32-bit indexed draws, with indices and positions both read from CPU-visible pool memory; the scissor / viewport / geometry / winding / depth clipping decisions are in nvn_cover.h (pure functions, with unit tests)
// Read float2/3/4 or half2/3/4 attribute a of all vertices (non-indexed or 8 / 16 / 32-bit indexed; vertex number = first + k or index value + base_vertex), with missing components filled with 0 / 1 per the fetch rules.
// Returns 0 on success, a reason code on failure (6 vertex count / index type, 7 attribute index, 8 format / stream, 9 out of bounds, 10 address)

int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c: where a descriptor pool's IDs start in the host table
static int state_op(uint32_t op, const void *a) {
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        // The pool bound when the draw runs, as on the GPU (a command buffer that binds none keeps the queue's): handles a shader reads from a uniform buffer are resolved only here,
        // so all handles take this base. Dark Souls Remastered keeps 12 sampler pools with different samplers under the same IDs and its map materials read their handles from uniform
        // memory; resolved in the first pool, the walls' tiling textures took a clamp-to-edge sampler and smeared sideways
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int tex = op == OP_SET_TEX_POOL, b = nvn_dpool_base(((const p_set_pool *)a)->pool, tex);
            if (b != (tex ? S.tex_base : S.samp_base)) { if (tex) S.tex_base = b; else S.samp_base = b; args_dirty_all(); }
            break; }
        // The compute stage has a binding of its own: a compute bind used to replace the one bound program, so a draw after a dispatch found the compute program (or none) and was
        // dropped, and a dispatch after a graphics bind found no compute shader. Hyrule Warriors: Age of Calamity interleaves them; its movie's NV12 → RGB draw was dropped and the intro played black
        case OP_BIND_PROGRAM: { const p_bind_program *bp_ = a;
            if ((bp_->stages & 0x20) && bp_->prog != S.cprog) { S.cprog = bp_->prog; args_dirty_all(); }
            if (gfx_stage_bits(bp_->stages)) { const prog_t *np = bind_stages(bp_->prog, gfx_stage_bits(bp_->stages)); if (np != S.prog) { S.prog = np; g_pso_dirty = 1; args_dirty_all(); } }
            break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; void *dst = NULL; size_t n = 0;
            blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &S.blend[bs.target]; n = sizeof bs; break;
                case 1: dst = &S.color; n = sizeof S.color; break;
                case 2: dst = &S.chmask; n = sizeof S.chmask; break;
                case 3: dst = &S.ms; n = sizeof S.ms; break;
                case 4: dst = &S.poly; n = sizeof S.poly; break;
                case 5: dst = &S.ds; n = sizeof S.ds; break;
            }
            if (dst && memcmp(dst, b->bytes, n)) { memcpy(dst, b->bytes, n); if (b->kind == 5) g_dss_dirty = 1; else g_pso_dirty = 1; }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (S.nattr != b->n || memcmp(S.attr, b->st, 4 * b->n)) { memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (S.nstream != b->n || memcmp(S.stream, b->st, 8 * b->n)) { memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (resource_range_fits(b->stage, b->index, 1, 16)) { S.ubo_set[b->stage] |= (uint16_t)(1u << b->index); if (memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; args_dirty_ubo(b->stage); } } break; }   // see encode_args_: a block whose other slots still hold is patched
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (resource_range_fits(b->stage, b->index, 1, 16) && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; args_dirty(b->stage); } break; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
            if (b->n < 0 || b->n > 32) {   // the recorder never writes more than 32 (bind_handles): the record was overwritten after it was recorded
                static int once; if (!once++) { const uint8_t *q = (const uint8_t *)a; char hx[3 * 96 + 1]; for (int k = 0; k < 96; k++) snprintf(hx + 3 * k, 4, "%02x ", q[k - 32]);
                    MLOG("[RED] bind record at %p (pass %llu) holds count %d: overwritten after recording; bytes from -0x20: %s", a, g_stat_pass + 1, b->n, hx); }
                break; }
            bh_fresh(b);
            if (passdump()) for (int k = 0; k < b->n; k++) { int tid = (int)(b->h[k] & 0xfffff); if (tid) PD("T %llu bind st%d slot%d tid=%d sid=%d ser=%llu%s", g_stat_pass + 1, b->stage, b->first + k, tid, (int)(b->h[k] >> 20),
                tid < g_ntreg ? g_treg[tid].t.serial : 0ull, b->defer ? " deferred" : ""); }
            if (resource_range_fits(b->stage, b->first, b->n, NTEX) && memcmp(&S.tex[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); args_dirty(b->stage); } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; bh_fresh(b); if (resource_range_fits(b->stage, b->first, b->n, 128) && memcmp(&S.samp[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); args_dirty(b->stage); } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; bh_fresh(b); if (resource_range_fits(b->stage, b->first, b->n, 32)) { if (memcmp(&S.img[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); args_dirty(b->stage); } } else RED_ONCE(31, "image slots out of range stage %d first %d count %d", b->stage, b->first, b->n); break; }
        case OP_STENCIL: {
            const p_stencil *b = a;
            if (b->which != 2) g_dss_dirty = 1;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; }
            break;
        }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a;
            for (int i = 0; i < b->n; i++) { memcpy(S.vp[b->first + i], b->v[i], 16); S.vp_origin[b->first + i] = b->origin; }
            S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_LINE_WIDTH: S.line_width = ((const p_line_width *)a)->w; break;
        case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; memcpy(S.vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_ALPHA_REF: S.alpha_ref = ((const p_alpha_ref *)a)->ref; break;   // data for at_bind, not part of the pipeline
        case OP_PUSH_CONST: { const p_push_const *b = a; memcpy(S.push[b->compute ? 1 : 0] + b->offset, b->data, b->size);
            for (int st = 0; st < 6; st++) if ((st == 5) == (b->compute != 0)) args_dirty(st); break; }
        case OP_COPY_STRIDE: { const p_copy_stride *cs = a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
        case OP_PRIM_RESTART: S.restart = *(const p_prim_restart *)a; break;
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; g_pso_dirty = 1; break;
        default: return 0;
    }
    return 1;
}

static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl);

// Prediction for parallel precompilation (see the note before pso_get): scan from after the current operation to the end of this submit, replaying only state; at a draw, compute the pipeline key from "the state at that time + that render pass's attachment formats" and dispatch those not yet built to the background.
// Whether a render pass actually attaches depth / stencil is only decided at replay (depth-only / stencil-only / neither attached are omitted), so one key is computed for each possible attach mode -- guessing too many only means wasted background compiles and does not affect the result.
// The shader libraries of the programs bound in the rest of the submission, loaded in parallel before the prediction walks it. The first submission in a new area binds a few
// hundred shaders the run has not loaded yet, and loading them one by one on the submit thread (library, functions, argument encoder, reflection) took about 1 s of the frame that
// entered Zora's Domain on a cold start (Breath of the Wild, M2 Max: 220 libraries). shader_load runs on the warm-up thread as well, so it may run off the submit thread; the results
// enter the cache here, on the submit thread, as shader_get enters them. With hybrid pipelines a vertex shader's pulling entry is loaded with it. Called by the first pipeline miss of
// a submission (pso_predict_rest), so a run whose shaders are all loaded never walks the commands for it
static os_unfair_lock g_pf_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t g_pf_n, g_pf_ns;
static void shader_prefetch_rest(void) {
    if (!g_exec_next || !g_shmap) return;   // no shader map loaded yet: no game shader resolved so far (the graphics tests register their programs themselves)
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NSMutableArray<NSString *> *keys = [NSMutableArray array]; NSMutableArray<NSNumber *> *stages = [NSMutableArray array]; NSMutableSet<NSString *> *seen = [NSMutableSet set];
    op_cursor cur = {.p = g_exec_next};
    for (int guard = 0; guard < 200000; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!op_next(&cur, &q, &hq, &aq)) break;
        if (hq.op != OP_BIND_PROGRAM) continue;
        const p_bind_program *bp_ = aq; const prog_t *pg = bp_->prog;
        if (!pg || (pg->serial && serial_get(pg->serial))) continue;   // resolved already
        for (unsigned i = 0; i < pg->nstage; i++) {
            uint8_t md[8]; if (!prog_key(pg, i, md)) MDIE("program stage %u has no shader key", i);
            NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
            NSString *stage = g_shmap[k]; int st = stage ? stage_of(stage) : -1;
            if (st < 0) continue;
            NSString *lk = [k stringByAppendingString:@"_b"];
            if (g_shcache[lk] || [seen containsObject:lk]) continue;
            [seen addObject:lk]; [keys addObject:lk]; [stages addObject:@(st)];
        }
    }
    NSUInteger n = keys.count; if (n < 2) return;   // one alone loads as fast where its draw needs it
    uint64_t t0 = bp_now(); int hy = hybrid_on();
    NSMutableArray *res = [NSMutableArray arrayWithCapacity:n]; for (NSUInteger i = 0; i < n; i++) [res addObject:NSNull.null];
    dispatch_apply(n, DISPATCH_APPLY_AUTO, ^(size_t i) { @autoreleasepool {
        NvShader *o = shader_load(keys[i], stages[i].intValue);
        if (o && hy && o->stage == 0 && o->has_pull) pull_load(o);
        os_unfair_lock_lock(&g_pf_lock); if (o) res[i] = o; os_unfair_lock_unlock(&g_pf_lock);
    } });
    unsigned got = 0;
    for (NSUInteger i = 0; i < n; i++) { NvShader *o = res[i]; if ((id)o == NSNull.null) continue;
        g_shcache[keys[i]] = o; got++; progpf_first(keys[i]);
        pso_record("S\t%s\t%d\n", keys[i].UTF8String, stages[i].intValue); pso_user_record("S\t%s\t%d\n", keys[i].UTF8String, stages[i].intValue); }
    uint64_t dt = bp_now() - t0; g_bp_lib_n += got; g_bp_lib_ns += dt; g_pf_n += got; g_pf_ns += dt;
    static int logged; if (logged++ < 8) MLOG("shader prefetch: %u of %lu shader libraries the rest of the submission binds loaded in parallel in %.1f ms", got, (unsigned long)n, dt / 1e6);
}
// The pipeline of k needs no compile and no archive: its base is made, so its draw takes it for a specialization (pso_build_ex(..., 0), before any stand-in). The prediction asks
// this before it prepares a stand-in or hands a build over. Its candidates include each draw's key without the depth and stencil attachment, for a pass that attaches them on
// demand; a base does not depend on those formats, so such a key shares the base of the key the draws use, is rarely drawn, and so never enters g_pso. The prediction made a
// pulling stand-in for every one of them, each an archive lookup on the submit thread, in every run again: Ocarina of Time's castle town, 160 stand-ins / 104 ms in one frame.
// g_pp_easy: the keys this held for (submit thread; a base stays made for the process), which later predictions leave out of their candidates
static nvn_hmap g_pp_easy = {.klen = sizeof(pso_key)};
// *dp: the key's descriptor (nil without Metal 4 bases); *bkp (when given): its base's key, *bkp_ok set when there is one (not a whole pipeline, legal attachments)
static int pso_base_made_d(const pso_key *k, NvShader *vs, NvShader *fs, MTLRenderPipelineDescriptor **dp, pbase_key *bkp, int *bkp_ok) {
    if (dp) *dp = nil; if (bkp_ok) *bkp_ok = 0;
    pso4_init(); if (!g_comp4) return 0;
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0); if (dp) *dp = d;
    if ([d.label isEqualToString:PSO_WHOLE] || !pso_ds_valid((uint32_t)d.depthAttachmentPixelFormat, (uint32_t)d.stencilAttachmentPixelFormat)) return 0;
    pbase_key bk; pbase_key_of(d, pso_off(k, fs, 0), &bk); if (bkp) { *bkp = bk; *bkp_ok = 1; }
    os_unfair_lock_lock(&g_pbase_lock); int made = hm_get_id(&g_pbase, &bk) != nil; os_unfair_lock_unlock(&g_pbase_lock);
    return made;
}
static int __attribute__((unused)) pso_base_made(const pso_key *k, NvShader *vs, NvShader *fs) { return pso_base_made_d(k, vs, fs, NULL, NULL, NULL); }   // the tests' (t_pso_depth_stencil)
// The bases of predicted keys, looked up in the background. With hybrid pipelines the prediction used to make the pulling stand-in of every proposed key whose base was not
// made yet, on the submit thread: an archive lookup of the pulling base and a specialization each, outside the budget of two builds, to learn whether its draw would wait.
// That cost the submit thread most at a first visit (Ocarina of Time's Hyrule Field and Castle Town).
// A lookup's result is the base, and a base found (g_pbase) or known missing (g_pbase_miss) holds for the process whichever thread looked it up: so the
// prediction now hands each proposed key's lookup to a background worker (pso_bg_look): the base of the key's own pipeline (pso_build_ex(..., 0), which its draw tries first),
// else its pulling base; when neither is there the draw will wait, so the worker admits the key's build within the prediction's budget (pso_pend_kick_d), and the pulling base is
// built for the pair's next miss (hy_base_kick), as hybrid_get does. The draw then finds its base made and only specializes it (tens of microseconds), or takes its stand-in
// from the pulling base made. A draw that comes while its lookup is still queued looks it up itself, and the worker then finds it made; one that comes while the worker holds
// the base's lookup waits for its archive result (PbaseBuild.looked), before any compile, and takes the base it found.
// g_pp_looked: the bases handed over (submit thread), by the key of the key's own base: a base serves every key that shares it (a key and its variant without depth and stencil)
// and is looked up once; workers: one serial queue per performance core (at most 4), at user-initiated QoS, for draws that may come in the same frame
static nvn_hmap g_pp_looked = {.klen = sizeof(pbase_key)};
static dispatch_queue_t bgl_queue(void) {
    static dispatch_queue_t q[4]; static int n; static _Atomic unsigned rr; static dispatch_once_t once;
    dispatch_once(&once, ^{ int pc = 0; size_t sz = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &sz, NULL, 0) || pc < 2) pc = 2;
        n = pc > 4 ? 4 : pc;
        for (int i = 0; i < n; i++) q[i] = dispatch_queue_create("dm.nvn.pso-look", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)); });
    return q[atomic_fetch_add(&rr, 1) % (unsigned)n];
}
static void pso_bg_look(const pso_key *k, MTLRenderPipelineDescriptor *full, MTLRenderPipelineDescriptor *pull, int off, int pull_off, int spec, PsoPredictBudget *budget) {   // the submit thread; spec: pso_pend_kick_d's
    pso_key kc = *k; g_bgl_n++;
    dispatch_async(bgl_queue(), ^{ @autoreleasepool {
        uint64_t t0 = bp_now(); NSError *e = nil;
        id<MTLRenderPipelineState> ps = pso_build_ex(full, off, &e, 0);
        if (!ps && pull) { ps = pso_build_ex(pull, pull_off, &e, 0); if (!ps) hy_base_kick(pull, pull_off); }
        if (ps) g_bgl_found++;
        else if (pso_pend_kick_d(&kc, full, spec, budget)) g_pp_admit++;   // a draw met at once is admitted whatever runs, a guess within the budget
        g_bgl_ns += bp_now() - t0; g_bgl_done++;
    } });
}
static void pso_predict_rest_(void);
static void pso_predict_rest(void) { uint64_t t0 = bp_now(); shader_prefetch_rest(); pso_predict_rest_(); g_pt_pred_n++; g_pt_pred_ns += bp_now() - t0; }
// state_op also sets the scissor, which lives in R, not S: it is restored with S. Without that the replay went on with the last scissor of the rest of the submission until its next
// OP_SCISSOR. OCTOPATH TRAVELER II (Unreal Engine 4.27) with pipelines built on demand: from Castti's opening flashback and the battlefield of Hikari's opening on, the picture was bright
// green and white, in every run with the prediction and in none without it (DM_NVN_PREDICT=0); a short prewarm list hid it, as fewer pipelines missed
static void pso_predict_rest_(void) {
    if (!g_exec_next) return;
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S);
    // Prediction replays OP_SCISSOR into R as well as the pipeline state in S.
    typeof(R.sc) sc0 = R.sc; int has_sc0 = R.has_sc;
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    uint32_t cf[8]; memcpy(cf, g_pass.cfmt, sizeof cf); uint32_t df = g_pass.dfmt, sf = g_pass.sfmt, samples = g_pass.samples, dfull = df, sfull = sf; int kicked0 = (int)g_pp_kicked, draws = 0;
    int targets_known = 1;
    op_cursor cur = {.p = g_exec_next};
    // Collect likely attachment keys before alternatives, then admit only a small batch. Global dispatch and Metal do not promise compile order.
    enum { QN = 160 }; static struct { pso_key k; void *vs, *fs; } qa[QN], qb[QN]; int na = 0, nb = 0;
    for (int guard = 0; guard < 200000 && na < QN; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!op_next(&cur, &q, &hq, &aq)) break;
        if (hq.op == OP_SET_RT) {
            const p_set_rt *r = aq; memset(cf, 0, sizeof cf); dfull = sfull = 0; samples = 1; targets_known = 1;
            // Only inspect textures already made. Resolving a future attachment can upload it and wait for the GPU, ending the encoder of the draw asking for a pipeline.
            for (int i = 0; i < r->n && i < 8; i++) {
                const tex_t *color = r->color[i];
                if (!color || rt_dup_slot(r, i) || nvn_fmt_bc(color->d.format)) continue;   // BC targets are never attached directly (see attach)
                id<MTLTexture> t = tex_made(color);
                if (!t) { targets_known = 0; break; }
                cf[i] = (uint32_t)rt_view_fmt_predict(color, t, &r->cview[i], r->has_cview[i]); samples = (uint32_t)t.sampleCount;
            }
            id<MTLTexture> dt = r->depth ? tex_made(r->depth) : nil;
            if (r->depth && !dt) targets_known = 0;
            if (dt) { MTLPixelFormat f = dt.pixelFormat; samples = (uint32_t)dt.sampleCount;
                dfull = f != MTLPixelFormatStencil8 ? (uint32_t)f : 0; sfull = (f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8) ? (uint32_t)f : 0; }
            df = dfull; sf = sfull; continue;
        }
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) {
            if (!targets_known || S.rast_discard) continue;   // unresolved attachments and non-rasterizing pipelines are handled at draw time
            draws++;
            __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) continue;
            __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
            const uint32_t dv[4] = {dfull, 0, 0, dfull}, sv[4] = {sfull, 0, sfull, 0};
            for (int v = 0; v < 4; v++) {
                if (v && dv[v] == dv[0] && sv[v] == sv[0]) continue;
                if (v == 2 && !sfull) continue; if (v == 3 && (!dfull || !sfull)) continue;
                pso_key k; pso_key_fill(&k, vs, fs, cf, dv[v], sv[v], samples, 0);
                // A key with a stand-in already drew with it and asked for its own build then (pso_pend_demand): it is no candidate. Without this a prediction proposed
                // each such key again until its build landed, a pso_base_made descriptor on the submit thread each (issue #2438)
                if (hm_get_id(&g_pso, &k) || hm_get_id(&g_pp_easy, &k) || hm_get_id(&g_pso_hy, &k)) continue;
                int dup = 0; for (int i = 0; i < (v ? nb : na) && !dup; i++) dup = !memcmp(&(v ? qb : qa)[i].k, &k, sizeof k);
                if (dup) continue;
                if (!v && na < QN) { qa[na].k = k; qa[na].vs = (__bridge void *)vs; qa[na].fs = (__bridge void *)fs; na++; }
                else if (v && nb < QN) { qb[nb].k = k; qb[nb].vs = (__bridge void *)vs; qb[nb].fs = (__bridge void *)fs; nb++; }
            }
            continue;
        }
        if (hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    (void)df; (void)sf;
    // Demand first: this runs after the draw's own pipeline is built. A predicted key whose base is made needs no compile (pso_base_made) and takes nothing here. With hybrid
    // pipelines a predicted draw whose pulling stand-in is ready will not wait, and builds its own pipeline on demand (pso_pend_demand). The rest are draws that would wait (a pair
    // whose pulling base is not made yet): each draw met with its attachments as they are (qa) is admitted, then at most PSO_PREDICT_BUDGET guesses with other depth / stencil
    // attachments (qb). With hybrid pipelines a key whose base is not made has its lookup handed to the background (pso_bg_look), which admits its build when its draw would wait,
    // as the same rule says (a draw met at once, a guess within the budget); the submit thread makes no stand-in and asks no archive for it
    int admitted = 0, hy = hybrid_on();
    PsoPredictBudget *budget = [PsoPredictBudget new];
    for (int i = 0; i < na + nb && (i < na || admitted < PSO_PREDICT_BUDGET); i++) {
        const pso_key *qk = i < na ? &qa[i].k : &qb[i - na].k; NvShader *qv = (__bridge NvShader *)(i < na ? qa[i].vs : qb[i - na].vs), *qf = (__bridge NvShader *)(i < na ? qa[i].fs : qb[i - na].fs);
        if (hm_get_id(&g_pso, qk)) continue;
        MTLRenderPipelineDescriptor *d = nil; pbase_key bk; int bk_ok = 0;
        if (pso_base_made_d(qk, qv, qf, &d, &bk, &bk_ok)) { hm_put_id(&g_pp_easy, qk, NSNull.null); continue; }
        if (hy && d && bk_ok) {
            if (hm_get_id(&g_pp_looked, &bk)) continue;   // its base is being looked up, or was: found it is made by now, missing its build was admitted or its draw waits
            hm_put_id(&g_pp_looked, &bk, NSNull.null);
            pso_bg_look(qk, d, qv->has_pull ? pso_desc_pull(qk, qv, qf) : nil, pso_off(qk, qf, 0), qk->border ? 0 : FN_OFF_BORDER, i < na ? 2 : 1, budget);
            continue;
        }
        if (hy && hybrid_get(qk, qv, qf) != nil) continue;   // a whole pipeline (no base to look up): its stand-in, as before
        if (i < na) g_pp_admit += (uint64_t)pso_pend_kick_next(qk, qv, qf);
        else admitted += pso_pend_kick_budget(qk, qv, qf, 1, budget);
    }
    g_pp_admit += (uint64_t)admitted;
    memcpy(&S, &Ssave, sizeof S); R.sc = sc0; R.has_sc = has_sc0; args_dirty_all(); g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty; prog_resolve();
    static int logged; if (g_pp_kicked > (uint64_t)kicked0 && logged++ < 20) MLOG("parallel precompile: %d draws left in this submission, %d pipelines handed to the background", draws, (int)g_pp_kicked - kicked0);
}



// SetRenderTargets naming the targets of the open pass goes on in that pass. On a tile-based GPU a new pass loads and stores its whole attachments however little it draws; on the Switch the call costs nothing.
// Splatoon Raiders sets its 6400x6400 ink map again before each of about 150 occlusion queries a frame (one draw each, sampling the map, writing nothing): 150 passes of about 1 ms, 42 GB of attachment loads a frame,
// about 6 fps in the first level. What a pass end gave the draws around it is covered this way: sampling a colour attachment by fb_prepare; a game barrier by barrier_game; sampling the depth attachment (or what the host cannot see, bindless) ends the pass there, since
// its memory holds nothing of this pass until then; and a draw writing depth or stencil after a draw of the pass read that attachment ends it too (fb_prepare), since the GPU stores the pass tile by tile, so the earlier reads would see the later writes in the tiles already done.
// Its water reads the scene's depth and the next draw writes the water's own: without that rule, grey blocks of tiles all over the sea. A pass with no colour attachment ends as before. DM_NVN_RT_CONTINUE=0 ends every pass as before
static int rtc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_RT_CONTINUE"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_RT_CONTINUE=0: setting the same render targets again ends the pass"); } return v; }
// Two views select the same subresources and format: only the fields their flags set count (the rest, and _rsv, may hold anything the game left in its stack slot)
static int view_same(const view_t *x, const view_t *y) {
    if (x->set_levels != y->set_levels || x->set_layers != y->set_layers || x->set_format != y->set_format || x->set_target != y->set_target || x->set_swizzle != y->set_swizzle || x->set_dsmode != y->set_dsmode) return 0;
    if (x->set_levels && (x->base_level != y->base_level || x->nlevels != y->nlevels)) return 0;
    if (x->set_layers && (x->min_layer != y->min_layer || x->nlayers != y->nlayers)) return 0;
    if (x->set_format && x->format != y->format) return 0;
    if (x->set_target && x->target != y->target) return 0;
    if (x->set_swizzle && memcmp(x->swz, y->swz, 4)) return 0;
    if (x->set_dsmode && x->dsmode != y->dsmode) return 0;
    return 1;
}
static const char *g_rt_diff;   // why the last rt_same said no (the pass dump's M line)
static int rt_same(const p_set_rt *a, const p_set_rt *b) {   // Same set of targets: compare by texture serial and view (not by object pointer: the game may copy texture objects by value)
    int na = a->n, nb = b->n;   // trailing empty colour slots do not change the set
    while (na > 0 && !a->color[na - 1]) na--;
    while (nb > 0 && !b->color[nb - 1]) nb--;
    if (na != nb) { g_rt_diff = "n"; return 0; }
    if (!a->depth != !b->depth || (a->depth && a->depth->serial != b->depth->serial)) { g_rt_diff = "depth"; return 0; }
    if (a->depth) { view_t z = {0}; if (!view_same(a->has_dview ? &a->dview : &z, b->has_dview ? &b->dview : &z)) { g_rt_diff = "dview"; return 0; } }
    for (int k = 0; k < na && k < 8; k++) {
        if (!a->color[k] != !b->color[k] || (a->color[k] && a->color[k]->serial != b->color[k]->serial)) { g_rt_diff = "color"; return 0; }
        if (a->color[k]) { view_t z = {0}; if (!view_same(a->has_cview[k] ? &a->cview[k] : &z, b->has_cview[k] ? &b->cview[k] : &z)) { g_rt_diff = "cview"; return 0; } }
    }
    g_rt_diff = NULL; return 1;
}
// Slice-merged passes (see g_sm_plan). sm_rt_slice: b names the same single colour target as a (one colour attachment in slot 0, no depth / stencil, the same texture and view)
// except the view's first layer, which goes to *slice
static int sm_on(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_NVN_SLICE_MERGE"); m = !(e && !strcmp(e, "0")); if (!m) MLOG("DM_NVN_SLICE_MERGE=0: passes into other layers of one texture level stay apart"); } return m; }
static int sm_rt_slice(const p_set_rt *a, const p_set_rt *b, int *slice) {
    if (a->depth || b->depth || a->n < 1 || b->n < 1 || !a->color[0] || !b->color[0] || a->color[0]->serial != b->color[0]->serial || !a->has_cview[0] || !b->has_cview[0]) return 0;
    for (int k = 1; k < 8; k++) if ((k < a->n && a->color[k]) || (k < b->n && b->color[k])) return 0;
    view_t x = a->cview[0], y = b->cview[0];
    if (!x.set_layers || !y.set_layers || x.set_format || y.set_format || (x.set_target && x.target != NVN_TEXTURE_TARGET_2D) || (y.set_target && y.target != NVN_TEXTURE_TARGET_2D)) return 0;
    x.min_layer = y.min_layer = 0; if (memcmp(&x, &y, sizeof x)) return 0;
    *slice = b->cview[0].min_layer; return 1;
}
// From the op after the draw that opens a pass: the layers the following targets switch to while nothing but state, uniform updates (deferred in the pass: ubd_on) and draws comes between.
// Returns the number of switches; lo..hi and the mask of layers drawn into
static int sm_scan(const uint8_t *p, const p_set_rt *rt, int cur, int *lo, int *hi, uint64_t *mask) {
    int n = 0; *lo = *hi = cur; *mask = cur < 64 ? 1ull << cur : 0;
    for (int guard = 0; guard < 4096 && p; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        switch (h.op) {
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_SET_RT: { int sl; if (!sm_rt_slice(rt, a, &sl) || sl >= 64) return n;
                if (sl != cur) { n++; cur = sl; if (sl < *lo) *lo = sl; if (sl > *hi) *hi = sl; *mask |= 1ull << sl; } break; }
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4); if (bits) return n; break; }
            case OP_UPDATE_UB: if (!ubd_on()) return n; break;
            case OP_DRAW: case OP_DRAW_IDX: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM:
            case OP_BIND_VBUF: case OP_BIND_VBUF1: case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET:
            case OP_BLEND_COLOR: case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_DEPTH_CLAMP: case OP_LINE_WIDTH: case OP_VP_SWIZZLE: case OP_PRIM_RESTART: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: break;
            default: return n;
        }
        p += h.size;
    }
    return n;
}
// The draw about to open a pass: plan it as slice-merged when its target is one layer of an array or cube texture level and the next targets switch to other layers of that level
// (every layer between the lowest and the highest drawn into), and the draw's amplified pipeline is built (else it is queued, and this pass stays as it is)
static int draw_samples_rt(NvProg *pr); static int fb_mode(void); static int fb_depth_on(void);
static void sm_plan(NvProg *pr) {
    g_sm_plan.on = 0;
    if (!pr || !pr->sh[0] || R.clear_d || R.clear_s || draw_samples_rt(pr)) return;
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) return;
    int cur; if (!sm_rt_slice(&R.rt, &R.rt, &cur) || cur >= 64) return;
    id<MTLTexture> t = tex_mtl_replay(R.rt.color[0]);
    if (!t || t.sampleCount != 1 || mtl_fmt_block(t.pixelFormat) || mtl_fmt_depth(t.pixelFormat) || (t.textureType != MTLTextureType2DArray && t.textureType != MTLTextureTypeCube && t.textureType != MTLTextureTypeCubeArray)) return;
    int lo, hi; uint64_t mask; if (!sm_scan(g_exec_next, &R.rt, cur, &lo, &hi, &mask)) return;
    if (__builtin_popcountll(mask) != hi - lo + 1 || (NSUInteger)hi >= t.arrayLength * (t.textureType == MTLTextureType2DArray ? 1 : 6)) return;
    __typeof__(g_pass) keep = g_pass; memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pass.cfmt[0] = (uint32_t)t.pixelFormat; g_pass.dfmt = g_pass.sfmt = 0; g_pass.samples = 1;   // the key pso_get makes in the pass
    g_pso_amp = 1; id ps = pso_get(pr, nil); g_pso_amp = 0; g_pass = keep;
    if (!ps) { g_sm_nwait++; return; }
    g_sm_plan.on = 1; g_sm_plan.lo = lo; g_sm_plan.hi = hi;
}
// Face-split draws (DM_NVN_FACE_SPLIT, on by default; 0 off, ab alternates with DM_AB). A full-screen draw whose colour targets are several layers of one texture level, a target per
// layer, computes all of them in each fragment thread: Mario Kart World filters its reflection cube maps so (GearPassShader 4744589bb8f9f7dd, DrivePassShaderDefault 765844f5609bb11b), each
// of the six outputs from its own face's direction with 32 cube samples, levels 64x64 down to 2x2, three chains a frame. A 64x64 level is 128 SIMD groups of 192 samples each, which the GPU
// cannot keep enough of in flight. Drawn once per layer instead, each draw with a pipeline that has only that output (the compiler drops the others' work: pso_build_ex PSO_FACE) and
// sends it to the one layered attachment (the encoder's colour attachment map) at its layer (the vertex amplification view mapping slice-merged passes use), the same samples run in six
// times the threads. The pixels are the same: each output is the same function of the same inputs, written to the same texel. Only for a draw that cannot tell: a few vertices and one
// instance, no blending, alpha to coverage, logic operation or alpha test, no depth or stencil target, no discard or sample mask, nothing written but its colour outputs (a storage or
// image write would happen once per layer), every target written, no read of its own targets, and a texture whose layers the pass can attach together. The pass holds that one draw
// (rp_begin_ex, then rp_end). docs/reference/rendering.md, "A draw into the six faces of a cube level at once ran short of threads"
static void fsp_plan(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    g_fsp_plan.on = 0;
    static int mode = -1; if (mode < 0) { mode = sw_mode("DM_NVN_FACE_SPLIT", 1); if (!mode) MLOG("DM_NVN_FACE_SPLIT=0: a draw into several layers of one texture level keeps a target per layer"); }
    int n = 0; while (R.has_rt && n < R.rt.n && n < 8 && R.rt.color[n]) n++;   // the leading targets; none after them
    if (!sw_on(mode) || !pr || !pr->sh[0] || !pr->sh[1] || pr->sh[3] || pr->sh[4] || !R.has_rt || R.rt.depth || n < 4) return;
    for (int i = n; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) return;
    if (S.rast_discard || S.ms.a2c || S.color.blend_en || R.clear_d || R.clear_s || g_pso_lmap) return;
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) return;
    if (dw ? dw->ind || dw->mdi_cnt || dw->base_instance || dw->count < 3 || dw->count > 6 || dw->instances > 1 || (dw->prim != 4 && dw->prim != 5)
           : di->ind || di->mdi_cnt || di->count < 3 || di->count > 6 || di->instances > 1 || (di->prim != 4 && di->prim != 5)) return;
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!fs->rf.color_known || fs->rf.may_discard || fs->rf.writes_smask) return;
    for (int st = 0; st < 2; st++) for (int i = 0; i < pr->sh[st]->rf.nb; i++) if (pr->sh[st]->rf.b[i].rw) return;
    for (int i = 1; i < n; i++) if (R.rt.color[i]->serial != R.rt.color[0]->serial) return;   // the views' formats: rp_begin_ex checks the attachments
    if (draw_samples_rt(pr)) return;
    id<MTLTexture> t = tex_mtl_replay(R.rt.color[0]);
    if (!t || t.sampleCount != 1 || mtl_fmt_block(t.pixelFormat) || mtl_fmt_depth(t.pixelFormat) || pf_int_kind(t.pixelFormat)
        || (t.textureType != MTLTextureType2DArray && t.textureType != MTLTextureTypeCube && t.textureType != MTLTextureTypeCubeArray)) return;
    pso4_init(); if (!g_comp4) return;   // the colour attachment map needs Metal 4 pipelines (pso_build_ex PSO_FACE)
    if (!g_fsp_map[0]) for (int i = 0; i < 8; i++) { g_fsp_map[i] = [MTLLogicalToPhysicalColorAttachmentMap new]; [g_fsp_map[i] setPhysicalIndex:0 forLogicalIndex:(NSUInteger)i]; }
    int wait = 0;
    for (int i = 0; i < 8; i++) g_fsp_ps[i] = nil;
    for (int i = 0; i < n; i++) if (!(fs->rf.color_mask >> i & 1) || !((S.chmask.mask >> (4 * i)) & 0xf)) return;   // every target written: the pass's store follows its one attachment's writes (sd_finish)
    // The pipelines of the draw before, when its face 0 key is this one's: a face's key differs from face 0's only in where the format stands, so the others are the same too. A cube
    // filter chain draws its levels with one program, and the six key fills and lookups a draw made were about 1 % of the replay thread in a Mario Kart World race (M3 Max)
    static pso_key last; static int last_n; static __unsafe_unretained id<MTLRenderPipelineState> last_ps[8];
    for (int i = 0; i < n; i++) {
        uint32_t cf[8] = {0}; cf[i] = (uint32_t)t.pixelFormat;
        pso_key k; pso_key_fill(&k, vs, fs, cf, 0, 0, 1, 0);
        if (k.logic || k.atest) return;
        k.fsfn = FSP_MARK;
        if (!i && last_n == n && !memcmp(&k, &last, sizeof k)) { for (int q = 0; q < n; q++) g_fsp_ps[q] = last_ps[q]; break; }   // NOLINT(bugprone-suspicious-memory-comparison): pso_key_fill zeroes the whole key first
        if (!i) last = k;
        g_fsp_ps[i] = sm_amp_get(&k, vs, fs); wait |= !g_fsp_ps[i];
    }
    last_n = wait ? 0 : n; if (!wait) for (int q = 0; q < n; q++) last_ps[q] = g_fsp_ps[q];
    if (wait) return;   // built in the background (sm_amp_get): this draw keeps its targets
    g_fsp_plan.on = 1; g_fsp_plan.n = n; g_fsp_plan.fmt = (uint32_t)t.pixelFormat;
}
// Experimental RT range candidate: only ignore positive nlevels differences not used by the actual render attachments; other fields stay word-for-word identical.
static int tex_storage_overlap(const tex_t *a, const tex_t *b) {
    if (!a || !b) return 0;
    if (a->serial == b->serial) return 1;
    if (!a->mem || !b->mem) return 0;
    uint64_t x = (uint64_t)a->mem, y = (uint64_t)b->mem;
    if (!a->L.size || !b->L.size || a->L.size > UINT64_MAX - x || b->L.size > UINT64_MAX - y) return 1;
    return (x < y + b->L.size && y < x + a->L.size) || nvn_sparse_alias(x, x + a->L.size) || nvn_sparse_alias(y, y + b->L.size);
}
static int g_samples_fb_left;   // draw_samples_rt: a fragment texture that is a colour attachment does not count: fb_prepare answers for it, as for any draw of a pass
static const char *g_dsr_why;   // what draw_samples_rt found (the pass dump's split line)
#define DSR(w) do { g_dsr_why = (w); return 1; } while (0)
static int draw_samples_rt(NvProg *pr) {   // Keep storage-alias guards for resolved handles as well as ordinary slots
    g_dsr_why = NULL;
    if (!pr) DSR("noprog");
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        int handles = st <= 1 && g_samples_fb_left && fb_mode();   // Resolve vertex and fragment uniform handles; slice merging keeps its conservative guard.
        if (sh->rf.dyn || sh->rf.pool || (sh->rf.bindless && !handles)) DSR("unknown-handles");
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i];
            // A raw/storage buffer can read an attachment's pool bytes, including a mirror written only at pass end. Preserve the old bindless fallback without a disjoint-range proof.
            if (sh->rf.bindless && (b->kind == NVN_RB_GMEM || b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_SBUFU || b->kind == NVN_RB_SBUFQ)) DSR("bindless-buffer");
            // Tables, pool reads and uniform image/buffer handles have no complete attachment proof here. Keep the original split.
            if (b->kind == NVN_RB_TEXD || b->kind == NVN_RB_TEXA || b->kind == NVN_RB_TEXH || b->kind == NVN_RB_TEXP || b->kind == NVN_RB_IMGU || b->kind == NVN_RB_TBUFU) DSR("unknown-binding");
            int texu = b->kind == NVN_RB_TEXU;
            if (texu && !handles) DSR("texu");
            if (!texu && b->kind != NVN_RB_IMG && b->kind != NVN_RB_TEX && b->kind != NVN_RB_TBUF) continue;
            for (int k = 0, n = texu && b->n ? b->n : 1; k < n; k++) {
                uint64_t h = texu ? 0 : b->kind == NVN_RB_IMG ? (b->reg < 32 && sh->stage < 6 ? bound_img(sh->stage, b->reg) : 0) : (sh->stage < 6 ? bound_tex(sh->stage, b->reg) : 0);
                int tid = texu ? ubo_tex_id(b->n ? ubo_handle(st, b->reg, b->off + 8 * k) : ubo_bind_handle(st, b)) : (int)(h & 0xfffff);
                if (!tid) continue;   // an unbound handle reads the dummy, as in encode_args
                os_unfair_lock_lock(&g_rmu); tex_t t = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t : (tex_t){0}; os_unfair_lock_unlock(&g_rmu);
                if (!t.serial) DSR("unreg");
                // Ordinary fragment slots' colour feedback uses fb_prepare. A uniform handle naming colour stays conservative.
                int colour_slot = 0;
                for (int j = 0; j < R.rt.n && j < 8; j++) if (R.rt.color[j]) {
                    if (g_samples_fb_left && st == 1 && b->kind == NVN_RB_TEX && R.rt.color[j]->serial == t.serial) colour_slot = 1;
                    else if (tex_storage_overlap(&t, R.rt.color[j])) DSR("color-alias");
                }
                if (colour_slot) continue;
                if (st == 1 && R.rt.depth && R.rt.depth->serial == t.serial && handles && fb_depth_on() && (texu || b->kind == NVN_RB_TEX)) continue;   // fb_depth_rule orders only fragment reads
                if (tex_storage_overlap(&t, R.rt.depth)) DSR("depth-alias");
            }
        }
    }
    return 0;
}
#undef DSR
// The whole render pass's need for depth / stencil (bits: 1 depth 2 stencil; 3 = both needed or cannot be proven). Starting from the draw that opens the render pass, scan in real replay order to the end of the render pass:
// the attached clears and this draw's state are counted first; later draws accumulate by their depth-stencil state; anything unprovable such as a mid-pass clear or nesting too deep is treated as both needed
// Pure-clear render pass hoisting: when a render pass is opened, scan forward, and if the shape is "several draws of this render pass -> several groups (SetRenderTargets + only clears) -> back to the same targets as this render pass",
// then hoist those groups of clears to execute before this render pass and skip them at their original positions, and on returning to the same targets the same-target continuation picks up -- this render pass's attachments are no longer stored whole and loaded again for the clears sandwiched in between
// (character select page: grunge and particles both draw 28 / 27 level 0 + depth, with only levels 1 and 2 cleared in between).
// Equivalence conditions (hoist only if all hold, otherwise leave as-is): the scan range has only draws, state and these clears, no copies / dispatches / texture clears / sub-lists; the cleared subresources (texture + level + layer)
// are disjoint from this render pass's attachments; the cleared textures are neither on any current texture / image slot nor bound within the scan range (conservative: regardless of whether the program samples them); the scissor at the time of the clear covers the whole block (same requirement as executing at the original position)
// A texture ID's serial, 0 when it holds no texture: the lock-free copy mtl_reg_tex writes beside the registration (g_tser), so the per-draw readers (the same-pass
// feedback checks) take no g_rmu for it
static inline uint64_t tid_ser(int tid) { return tid > 0 && tid < (1 << 20) ? atomic_load_explicit(&g_tser[tid], memory_order_relaxed) : 0; }
static uint64_t h_ser(uint64_t h) { return tid_ser((int)(h & 0xfffff)); }
static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl) {   // Subresource of attachment k (0–7 color, 8 depth)
    const tex_t *t = k == 8 ? r->depth : r->color[k]; const view_t *v = k == 8 ? &r->dview : &r->cview[k]; int hv = k == 8 ? r->has_dview : r->has_cview[k];
    *ser = t ? t->serial : 0; *lv = hv && v->set_levels ? v->base_level : 0; *sl = hv && v->set_layers ? v->min_layer : 0;
}
// The readable-tile version of fragment shader fs specialized by constants, kept on the shader and found by its numbers: fb_tile_plan asks at every draw that reads the tile (Breath of the
// Wild: about 270 a frame), and the string keys this was found by before took more time than the rest of those draws' translation (Mac, encode thread on: 15,900 instructions a draw, 11,900
// without them; PR #1161). A shader has a few specializations, so a scan of its own list is the lookup
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap) {
    int nfc = fs->rf.nfc < 32 ? fs->rf.nfc : 32;
    bf_key k; memset(&k, 0, sizeof k); for (int i = 0; i < nfc; i++) k.fx[i] = (int8_t)fx[i];
    k.fbmask = fbmask; memcpy(k.lmap, lmap, 8);
    os_unfair_lock_lock(&g_bf_fn_lock);
    for (NvBfFn *s in fs->bffn) if (!memcmp(&s->k, &k, sizeof k)) { id<MTLFunction> f = s->fn; os_unfair_lock_unlock(&g_bf_fn_lock); return f; }
    if (!fs->bflib_state) {   // loaded once (a failure too: not again for every draw)
        size_t kl = strlen(fs->key); NSError *e = nil;
        NSString *path = [NSString stringWithFormat:@"%@/%.*s_bf.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), fs->key];
        fs->bflib = lib_from_file(path, &e); fs->bflib_state = fs->bflib ? 1 : 2;
        if (!fs->bflib) MLOG("[RED] failed to load the readable-tile version %s: %s", path.UTF8String, e.localizedDescription.UTF8String);
    }
    id<MTLLibrary> lib = fs->bflib;
    if (!lib) { os_unfair_lock_unlock(&g_bf_fn_lock); return nil; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    for (int i = 0; i < nfc; i++) { int v = fx[i]; [cv setConstantValue:&v type:MTLDataTypeInt atIndex:(NSUInteger)i]; }
    for (int q = 0; q < 8; q++) { bool b = fbmask >> q & 1; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(32 + q)]; }
    for (int j = 0; j < fs->rf.nfo; j++) for (int q = 0; q < 8; q++) { bool b = lmap[q] == fs->rf.fo_color[j]; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(64 + 8 * j + q)]; }
    NSError *e = nil; id<MTLFunction> f = fn4_note(dev_fn(lib, @"main0_f", cv, &e), lib, @"main0_f", cv);
    if (!f) MLOG("[RED] readable-tile version specialization failed %s (mask %02x): %s", fs->key, fbmask, e.localizedDescription.UTF8String);
    NvBfFn *s = [NvBfFn new]; s->k = k; s->fn = f;
    if (!fs->bffn) fs->bffn = [NSMutableArray array];
    [fs->bffn addObject:s]; os_unfair_lock_unlock(&g_bf_fn_lock); return f;
}
// Component cmp of vector vec of the uniform slot as the draw reads it: the last write queued in this submission when that is an inline update covering it (pend_read), else memory
// when nothing queued writes it; not read otherwise. Splatoon Raiders sets its ink stamps' coordinate scale by an inline update before each stamp
// A uniform reference: constant buffer slot << 16 | vec4 index (0..4095, a 64 KB buffer) << 2 | component (nvn_refl.m)
static int ubo_float(int slot, uint32_t ref, float *out) {
    int sl = ref >> 16, vec = ref >> 2 & 4095, cmp = ref & 3, kind;
    if ((unsigned)slot >= NVN_RESOURCE_STAGES || (unsigned)sl >= 16) return 0;
    const p_bind_buf *u = &S.ubo[slot][sl]; uint64_t off = (uint64_t)vec * 16 + (uint64_t)cmp * 4;
    if (!u->addr || off + 4 > u->size) return 0;
    if (pend_read(u->addr + off, 4, out, &kind)) return 1;
    if (pend_hits(u->addr + off, 4)) return 0;
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, 4); return 1;
}
// While an occlusion query is counting, a skipped draw should have contributed passed samples: it only does not matter if "there is no report before the next reset".
// Looking forward in replay order from after this draw: sample count reset encountered first -> does not matter (return 1); sample count report encountered first, or the end cannot be seen (submit ends) -> matters (return 0)
// A pixel-position sample (dmsh_fetch.py 'frag_pos', gl_FragCoord.xy * (U, V)) reads this pixel's texel center when U and V, as this draw reads them, are 1 / the W x H target's size
// (within 1/256 of a texel at the far edge). gl_FragCoord is the pixel center whatever the viewport, and a lower-left window origin stores the image bottom-up, so its rows and
// gl_FragCoord.y move together. Splatoon Raiders' ink stamps read their 6400x6400 map at gl_FragCoord * (1/6400) (GitHub #1063)
static int fpos_proof(NvShader *fs, int c, NSUInteger W, NSUInteger H) {
    float u, v;
    if (c < 0 || c >= 32 || !(fs->rf.fc_fpos >> c & 1) || !W || !H || !ubo_float(1, fs->rf.fc_scale[c][0], &u) || !ubo_float(1, fs->rf.fc_scale[c][1], &v)) return 0;
    return fabs((double)u * W - 1) * W < 1.0 / 256 && fabs((double)v * H - 1) * H < 1.0 / 256;
}
static unsigned mtl_pf_chmask(MTLPixelFormat pf);
// A sampled view whose channel mapping gives what a read of the tile gives: the format's own channels in place, and those it lacks either as they are or as the 0, 0, 1 a sample fills
// in, which a tile read fills in too (t_feedback checks RG8 on the GPU; an M4 Pro gives 0, 0, 1 for every R and RG format). Splatoon Raiders' RG8 ink map is sampled through (red, green, zero, one)
static int sw_tile(id<MTLTexture> t) {
    MTLTextureSwizzleChannels s = t.swizzle; MTLTextureSwizzle a[4] = {s.red, s.green, s.blue, s.alpha}; unsigned has = mtl_pf_chmask(t.pixelFormat);
    for (int q = 0; q < 4; q++) if (a[q] != (MTLTextureSwizzle)(MTLTextureSwizzleRed + q) && ((has >> q & 1) || a[q] != (q == 3 ? MTLTextureSwizzleOne : MTLTextureSwizzleZero))) return 0;
    return 1;
}
// A projective sample (dmsh_fetch.py 'proj', in.m_X.xy / in.m_X.w) reads this pixel's texel center: X is the vertex shader's clip → screen varying (scr_locn, dmsh_fetch.py screen_varyings),
// nothing between it and the rasterizer (no tessellation or geometry stage), the viewport is the whole W x H target with no swizzle, and the window origin is the upper left (lower left
// flips the viewport). Nothing is read from the draw's vertices: the varying is this fragment's position by construction, up to the interpolation's rounding (the sample
// lands within float precision of the texel center, which the tile read returns exactly)
static int scr_proof(NvProg *pr, int locn, NSUInteger W, NSUInteger H) {
    NvShader *vs = pr->sh[0];
    if (!vs || locn < 0 || locn >= 32 || !(vs->rf.scr_locn >> locn & 1) || pr->sh[3] || pr->sh[4]) return 0;
    if (g_queue_origin_ll || vp_origin_ll()) return 0;
    if (S.vp_sw[0][0] || S.vp_sw[0][1] || S.vp_sw[0][2] || S.vp_sw[0][3]) return 0;
    return !S.has_vp || (S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)W && S.vp[0][3] == (float)H);
}
// One draw of the later pass: verify and produce the specialization. Returns 0 = cannot be proven (the caller ends the merged render pass and reopens with the original targets)
// Diagnostic DM_NVN_KEYDUMP=<fragment shader key prefix>[:<count, default 6>[:<frame interval>]]: watch only this class of draws (for real devices; no frame number to guess). The first N matching draws force PASSDUMP's per-binding lines on (K / B / d:
// textures with format / view / channel swizzle, samplers with LOD clamps), plus [kd] lines: vertex attribute formats, each vertex stream (first 4 vertices of per-vertex streams, all instances of per-instance streams) and every value of each uniform buffer of both stages.
// Run the same switch on two machines and compare item by item to see which input differs (Super Mario 3D World + Bowser's Fury's magenta squares in 1-1: particle 5e4de7561c670fee)
static void kd_floats(const char *what, int reg, uint64_t addr, uint64_t size, uint64_t cap) {
    void *mb; uint64_t mo; const uint8_t *base;
    if (!addr || !size) { fprintf(stderr, "[kd]   %s r%d unbound\n", what, reg); return; }
    if (!mtl_addr(addr, 1, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { fprintf(stderr, "[kd]   %s r%d addr=%#llx not in a memory pool\n", what, reg, (unsigned long long)addr); return; }
    uint64_t n = MIN(size, cap) / 4; const float *v = (const float *)(base + mo);
    fprintf(stderr, "[kd]   %s r%d addr=%#llx size=%llu%s\n", what, reg, (unsigned long long)addr, (unsigned long long)size, pend_hits(addr, size) ? " (pending writes in this submission)" : "");
    for (uint64_t i = 0; i < n; i += 16) {
        char line[1024]; size_t o = 0;
        for (uint64_t k = i; k < n && k < i + 16 && o < sizeof line - 24; k++) o += snprintf(line + o, sizeof line - o, " %.6g", v[k]);
        fprintf(stderr, "[kd]     +%#06llx%s\n", (unsigned long long)(i * 4), line);
    }
}
static int kd_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    static char key[24]; static int left = -1, every; static uint64_t last = ~0ull;
    if (left < 0) { left = 0; const char *e = getenv("DM_NVN_KEYDUMP");
        if (e && *e) { size_t n = strcspn(e, ":"); if (n > 23) n = 23; memcpy(key, e, n); key[n] = 0; const char *c = strchr(e, ':'); left = c ? atoi(c + 1) : 6; every = c && strchr(c + 1, ':') ? atoi(strchr(c + 1, ':') + 1) : 0;
            MLOG("DM_NVN_KEYDUMP: fragment shader %s, %d draws logged in detail%s ([kd] / [pass] lines)", key, left, every ? ", at most once every that many frames" : ""); } }
    if (left <= 0 || !pr->sh[1] || strncmp(pr->sh[1]->key, key, strlen(key))) return 0;
    uint64_t fr = atomic_load(&g_cur_frame);
    if (every > 0 && last != ~0ull && fr < last + (uint64_t)every) return 0;   // at most once every `every` frames: samples spread over the session, no need to guess the frame of the scene
    last = fr; left--;
    int count = dw ? dw->count : di->count, inst = dw ? dw->instances : di->instances, first = dw ? dw->first : 0, binst = dw ? dw->base_instance : di->base_instance;
    fprintf(stderr, "[kd] F%llu pass %llu vs=%s fs=%s prim %d vertices %d instances %d first %d base_vertex %d base_instance %d target %s\n", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass,
            pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1]->key, dw ? dw->prim : di->prim, count, inst, first, di ? di->base_vertex : 0, binst, R.rt.n && R.rt.color[0] ? "yes" : "no");
    for (int i = 0; i < S.nattr && i < 32; i++) fprintf(stderr, "[kd]   attr %d format %#x offset %u stream %u\n", i, (unsigned)S.attr[i].fmt, (unsigned)S.attr[i].offset, (unsigned)S.attr[i].stream);
    for (int s_ = 0; s_ < S.nstream && s_ < 16; s_++) {
        uint32_t st = S.stream[s_].stride, dv = S.stream[s_].divisor;
        fprintf(stderr, "[kd]   stream %d stride %u divisor %u buffer %#llx size %llu\n", s_, st, dv, (unsigned long long)S.vbuf[s_].address, (unsigned long long)S.vbuf[s_].size);
        if (!S.vbuf[s_].address || !st) continue;
        uint64_t from = (uint64_t)(dv ? binst : (di ? di->base_vertex : first)) * st, n = (uint64_t)st * (dv ? (uint64_t)MIN(inst, 8) : 4);
        if (from < S.vbuf[s_].size) kd_floats("stream", s_, S.vbuf[s_].address + from, MIN(n, S.vbuf[s_].size - from), 512);
    }
    for (int stg = 0; stg < 2; stg++) { NvShader *sh = pr->sh[stg]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_CBUF && sh->rf.b[i].reg < 16) {
            const p_bind_buf *u = &S.ubo[stg][sh->rf.b[i].reg]; kd_floats(stg ? "fragment uniform" : "vertex uniform", sh->rf.b[i].reg, u->addr, u->size, 4096); } }
    return 1;
}
// Diagnostic DM_NVN_MAGSCAN=<fragment key>:<scan every N frames, default 30>:<frames per phase, default 600>: a self-check for the magenta squares (Super Mario 3D World + Bowser's Fury 1-1).
// In a scan frame, as soon as each render pass ends, a compute kernel reads its color attachments (read goes through the same format decode as the game's shader samples) and gathers per 16x16 block: the magenta pixel count (R and B both >= 0.8 of the largest channel, G <= 0.1 of it),
// the non-finite pixel count and each channel's min / max. An attachment with a block holding one single magenta value throughout (exactly what the squares were, aligned to the 16-pixel grid) or with non-finite values gets a [mag] line (index of the pass within the frame, distance from the pass holding this particle, attachment serial / format / size, block position and value).
// Four phases rotate: phase 0 draws as usual, phase 1 drops every draw of this fragment shader, phases 2 / 3 keep the geometry and replace the fragment output with a constant (grey / bright magenta). Comparing the phases tells whether this draw causes it
// and whether its fragment output (texture reads, value range) matters; the first pass where the magenta blocks show tells which step goes wrong
static char g_ms_key[24]; static int g_ms_every, g_ms_plen, g_ms_init;
static int ms_on(void);
static const char *const k_ms_phase[4] = {"normal", "shader dropped", "shader outputs constant gray", "shader outputs constant bright magenta"};
static int ms_phase_now(void) { return (int)(atomic_load(&g_cur_frame) / (uint64_t)g_ms_plen % 4); }
// Phases 2 / 3: this shader's draws keep their geometry and the fragment stage is replaced by a constant output (grey 0.5 / bright magenta 1500,2,1500). The pipeline cache key carries the replacement function, kept apart from the original pipeline; these pipelines are not recorded in the prewarm list
static id<MTLFunction> ms_fs_override(NvShader *fs) {
    if (!fs || !ms_on() || strncmp(fs->key, g_ms_key, strlen(g_ms_key))) return nil;
    int ph = ms_phase_now(); if (ph < 2) return nil;
    static id<MTLFunction> fn[2]; static int failed;
    if (!fn[0] && !failed) { NSError *e = nil; id<MTLLibrary> lib = dev_lib(@"#include <metal_stdlib>\nusing namespace metal;\n"
            "fragment float4 msg() { return float4(0.5, 0.5, 0.5, 1.0); }\nfragment float4 msm() { return float4(1500.0, 2.0, 1500.0, 0.6); }\n", &e);
        fn[0] = [lib newFunctionWithName:@"msg"]; fn[1] = [lib newFunctionWithName:@"msm"];
        if (!fn[0] || !fn[1]) { failed = 1; MLOG("[RED] DM_NVN_MAGSCAN: cannot compile the constant fragment shaders %s", e.localizedDescription.UTF8String); } }
    return failed ? nil : fn[ph - 2];
}
static int ms_on(void) {
    if (!g_ms_init) { g_ms_init = 1; const char *e = getenv("DM_NVN_MAGSCAN");
        if (e && *e) { size_t n = strcspn(e, ":"); if (n > 23) n = 23; memcpy(g_ms_key, e, n); g_ms_key[n] = 0;
            const char *c = strchr(e, ':'); g_ms_every = c ? atoi(c + 1) : 30; c = c ? strchr(c + 1, ':') : NULL; g_ms_plen = c ? atoi(c + 1) : 600;
            if (g_ms_every < 1) g_ms_every = 30; if (g_ms_plen < g_ms_every) g_ms_plen = 600;
            MLOG("DM_NVN_MAGSCAN: fragment shader %s; every %d frames, scans each pass's color attachments for magenta blocks; every %d frames, switches phase (0 normal / 1 drop this shader's draws / 2 it outputs constant gray / 3 it outputs constant bright magenta) ([mag] lines)", g_ms_key, g_ms_every, g_ms_plen); } }
    return g_ms_key[0] != 0;
}
static uint64_t g_ms_ppass = ~0ull, g_ms_pframe = ~0ull, g_ms_drawn, g_ms_dropped;
// returns 1 = this phase drops the draw
static int ms_draw(NvProg *pr) {
    if (!ms_on() || !pr->sh[1] || strncmp(pr->sh[1]->key, g_ms_key, strlen(g_ms_key))) return 0;   // a deferred draw returns before this when recorded and only arrives here when replayed: every draw passes exactly once
    uint64_t f = atomic_load(&g_cur_frame);
    if (f / (uint64_t)g_ms_plen % 4 == 1) { g_ms_dropped++; return 1; }
    g_ms_drawn++; if (g_ms_pframe != f) { g_ms_pframe = f; g_ms_ppass = g_stat_pass; }   // the pass where this particle first draws this frame
    return 0;
}
static const char k_ms_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void ms(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]], uint2 g [[thread_position_in_grid]], uint2 tg [[threadgroup_position_in_grid]],\n"
"               uint li [[thread_index_in_threadgroup]], uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]]) {\n"
"  threadgroup uint sh[8][8]; bool in = g.x < o[0] && g.y < o[1]; float4 c = in ? t.read(g) : float4(0); float m = max(c.r, max(c.g, c.b));\n"
"  bool bad = in && (isnan(c.r) || isnan(c.g) || isnan(c.b) || isinf(c.r) || isinf(c.g) || isinf(c.b));\n"
"  bool mag = in && !bad && m > 0.0 && c.r >= 0.8 * m && c.b >= 0.8 * m && c.g <= 0.1 * m;\n"
"  uint r = as_type<uint>(max(c.r, 0.0)), gg = as_type<uint>(max(c.g, 0.0)), b = as_type<uint>(max(c.b, 0.0));\n"
"  uint v[8] = {simd_sum(mag ? 1u : 0u), simd_sum(bad ? 1u : 0u), simd_min(in ? r : ~0u), simd_max(in ? r : 0u), simd_min(in ? gg : ~0u), simd_max(in ? gg : 0u), simd_min(in ? b : ~0u), simd_max(in ? b : 0u)};\n"
"  if (sl == 0) for (int k = 0; k < 8; k++) sh[si][k] = v[k];\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup); if (li) return;\n"
"  for (int s = 1; s < 8; s++) { v[0] += sh[s][0]; v[1] += sh[s][1]; for (int k = 2; k < 8; k += 2) { v[k] = min(v[k], sh[s][k]); v[k + 1] = max(v[k + 1], sh[s][k + 1]); } }\n"
"  device uint *q = o + 4 + 4 * (tg.y * o[2] + tg.x); q[0] = v[0] | v[1] << 16 | (v[2] == v[3] && v[4] == v[5] && v[6] == v[7] ? 1u << 31 : 0u); q[1] = v[3]; q[2] = v[5]; q[3] = v[7]; }\n";
#define MS_BUF (16u << 20)
#define MS_MAXE 256
typedef struct { uint64_t pass_idx, ser; int att, pdist, fmt; uint32_t w, h, off; } ms_ent;
typedef struct { __strong id<MTLBuffer> b; uint64_t frame, fence; uint32_t used; int n, busy, full; ms_ent e[MS_MAXE]; } ms_scan;
static ms_scan g_ms_s[2]; static int g_ms_cur = -1;   // which buffer this scan frame writes (-1 = not scanning this frame)
static uint64_t g_ms_curf = ~0ull, g_ms_first;
static __strong id<MTLTexture> g_ms_att[8]; static NSUInteger g_ms_lv[8], g_ms_sl[8]; static uint64_t g_ms_aser[8]; static int g_ms_na; static uint64_t g_ms_apass;
static int g_ms_seq = -1, g_ms_sc, g_ms_hitf, g_ms_maxblk, g_ms_pscan; static uint64_t g_ms_f0, g_ms_f1, g_ms_pmag;   // the phase being tallied (frame / frames per phase) and its totals
static void ms_poll(void) {
    for (int k = 0; k < 2; k++) { ms_scan *s = &g_ms_s[k]; if (!s->busy || g_ms_cur == k || s->fence > g_done) continue;
        int seq = (int)(s->frame / (uint64_t)g_ms_plen);
        if (seq != g_ms_seq) {
            if (g_ms_seq >= 0) MLOG("[mag] phase %d (%s) frames %llu–%llu: scanned %d frames, frames with uniform magenta blocks (a whole 16×16 block of one magenta value) %d, at most %d blocks in one frame; magenta pixels in the attachments of the shader's pass %.1f per frame (%d frames); this shader drawn %llu times, dropped %llu times so far",
                                    g_ms_seq % 4, k_ms_phase[g_ms_seq % 4], (unsigned long long)g_ms_f0, (unsigned long long)g_ms_f1, g_ms_sc, g_ms_hitf, g_ms_maxblk, g_ms_pscan ? (double)g_ms_pmag / g_ms_pscan : 0.0, g_ms_pscan,
                                    (unsigned long long)g_ms_drawn, (unsigned long long)g_ms_dropped);
            g_ms_seq = seq; g_ms_sc = g_ms_hitf = g_ms_maxblk = g_ms_pscan = 0; g_ms_pmag = 0; g_ms_f0 = s->frame; }
        g_ms_sc++; g_ms_f1 = s->frame; int fblk = 0, pseen = 0;
        for (int i = 0; i < s->n; i++) { const ms_ent *e = &s->e[i]; const uint32_t *o = (const uint32_t *)((const uint8_t *)s->b.contents + e->off);
            uint32_t tw = (e->w + 15) / 16, th = (e->h + 15) / 16; uint64_t bad = 0, mag = 0; int ublk = 0, mblk = 0, nl = 0; char line[1400]; size_t lo = 0;
            for (uint32_t t = 0; t < tw * th; t++) { const uint32_t *q = o + 4 + 4 * t; uint32_t nmag = q[0] & 0xffff, nbad = q[0] >> 16 & 0x7fff; bad += nbad; mag += nmag;
                uint32_t npx = MIN(16u, e->w - t % tw * 16) * MIN(16u, e->h - t / tw * 16);
                int uni = q[0] >> 31;   // the whole block holds one value
                int allm = nmag == npx;
                if (allm) mblk++; if (allm && uni && npx >= 64) ublk++;
                if (((allm && uni && npx >= 64) || nbad) && nl < 8 && lo < sizeof line - 120) { float r, g, b; memcpy(&r, &q[1], 4); memcpy(&g, &q[2], 4); memcpy(&b, &q[3], 4); nl++;
                    lo += snprintf(line + lo, sizeof line - lo, " [%u,%u%s max %g %g %g non-finite %u]", t % tw * 16, t / tw * 16, uni ? " uniform" : "", r, g, b, nbad); } }
            fblk += ublk; if (e->pdist == 0 && e->att == 0) { g_ms_pmag += mag; pseen = 1; }
            if (ublk || bad) MLOG("[mag] F%llu phase %llu pass %llu (shader's pass %+d) attachment %d ser=%llu format %d %ux%u: uniform magenta blocks %d (all-magenta blocks %d), non-finite pixels %llu%s", (unsigned long long)s->frame, (unsigned long long)(s->frame / (uint64_t)g_ms_plen % 4),
                                  (unsigned long long)e->pass_idx, e->pdist, e->att, (unsigned long long)e->ser, e->fmt, e->w, e->h, ublk, mblk, (unsigned long long)bad, line);
        }
        if (fblk) g_ms_hitf++; if (fblk > g_ms_maxblk) g_ms_maxblk = fblk; g_ms_pscan += pseen;
        if (s->full) MLOG("[mag] F%llu scan buffer full; the remaining passes of this frame were not scanned", (unsigned long long)s->frame);
        s->busy = 0; s->n = 0; s->used = 0; s->full = 0; }
}
void ms_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    g_ms_na = 0; if (!ms_on()) return;
    uint64_t f = atomic_load(&g_cur_frame);
    if (f != g_ms_curf) {   // a new frame: the previous scan frame's counts are processed once its submission completes
        g_ms_curf = f; g_ms_first = g_stat_pass + 1; if (g_ms_cur >= 0) g_ms_cur = -1;
        if (f % (uint64_t)g_ms_every == 0) { for (int k = 0; k < 2; k++) if (!g_ms_s[k].busy) { g_ms_cur = k; break; }
            if (g_ms_cur >= 0) { ms_scan *s = &g_ms_s[g_ms_cur];
                if (!s->b) { s->b = [g_dev newBufferWithLength:MS_BUF options:MTLResourceStorageModeShared]; if (!s->b) { g_ms_cur = -1; return; } res_add(s->b); }
                s->busy = 1; s->frame = f; s->n = 0; s->used = 0; s->full = 0; s->fence = 0; } } }
    ms_poll();
    if (g_ms_cur < 0) return;
    g_ms_apass = g_stat_pass + 1;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ms_att[g_ms_na] = t; g_ms_lv[g_ms_na] = pd.colorAttachments[i].level; g_ms_sl[g_ms_na] = pd.colorAttachments[i].slice; g_ms_aser[g_ms_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ms_na++; }
}
void ms_after(void) {
    if (!g_ms_na) return;
    int na = g_ms_na; g_ms_na = 0;
    if (g_ms_cur < 0 || g_stat_pass != g_ms_apass) { for (int i = 0; i < na; i++) g_ms_att[i] = nil; return; }
    static int failed; static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static int logged_att;
    if (failed) return;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = dev_lib(@(k_ms_msl), &e); id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"ms"] : nil;
        ps = fn ? dev_cps(fn, &e) : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; ad.maxBufferBindCount = 1; at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        if (!ps || !at) { failed = 1; MLOG("[RED] DM_NVN_MAGSCAN: cannot create the compute pipeline %s", e.localizedDescription.UTF8String); return; } }
    ms_scan *s = &g_ms_s[g_ms_cur]; int pdist = g_ms_pframe == s->frame && g_ms_ppass != ~0ull ? (int)((int64_t)g_stat_pass - (int64_t)g_ms_ppass) : 9999;
    for (int i = 0; i < na; i++) { id<MTLTexture> t = g_ms_att[i]; g_ms_att[i] = nil;
        MTLPixelFormat pf = t.pixelFormat;
        int ok = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGB10A2Unorm
              || pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRGBA16Unorm || pf == MTLPixelFormatRGBA32Float;
        if (!ok || !(t.usage & MTLTextureUsageShaderRead) || t.sampleCount > 1 || (t.width >> g_ms_lv[i]) < 64 || t.storageMode == MTLStorageModeMemoryless || (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray)) continue;
        if (pdist == 0 && !logged_att++) MLOG("[mag] attachment %d of the shader's pass: ser=%llu format %lu %lux%lu level %lu layer %lu storage %lu usage %#lx compression type %ld allowGPUOptimizedContents %d", i, (unsigned long long)g_ms_aser[i], (unsigned long)pf,
                                             (unsigned long)t.width, (unsigned long)t.height, (unsigned long)g_ms_lv[i], (unsigned long)g_ms_sl[i], (unsigned long)t.storageMode, (unsigned long)t.usage, (long)t.compressionType, (int)t.allowGPUOptimizedContents);
        uint32_t w = (uint32_t)MAX(t.width >> g_ms_lv[i], 1ul), h = (uint32_t)MAX(t.height >> g_ms_lv[i], 1ul), tw = (w + 15) / 16, th = (h + 15) / 16, need = 16 + tw * th * 16;
        if (s->n >= MS_MAXE || s->used + need > MS_BUF) { s->full = 1; continue; }
        id<MTLTexture> v = t;
        if (t.textureType != MTLTextureType2D || g_ms_lv[i] || g_ms_sl[i]) v = [t newTextureViewWithPixelFormat:pf textureType:MTLTextureType2D levels:NSMakeRange(g_ms_lv[i], 1) slices:NSMakeRange(g_ms_sl[i], 1)];
        if (!v) continue;
        uint32_t *o = (uint32_t *)((uint8_t *)s->b.contents + s->used); o[0] = w; o[1] = h; o[2] = tw; o[3] = 0;
        ms_ent *e = &s->e[s->n++]; e->pass_idx = g_stat_pass - g_ms_first; e->ser = g_ms_aser[i]; e->att = i; e->pdist = pdist; e->fmt = (int)pf; e->w = w; e->h = h; e->off = s->used; s->used += need;
        if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL); [ce setArgumentTable:at];
        [at setTexture:v.gpuResourceID atIndex:0]; at_addr(at, s->b.gpuAddress + e->off, 0, "magenta scan table slot", 0);
        enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake(tw, th, 1), MTLSizeMake(16, 16, 1)); enc_c_end(ce);
        args_dirty_all(); s->fence = g_fence + 1;
        if (v != t) defer_obj_unref(v);
    }
}
