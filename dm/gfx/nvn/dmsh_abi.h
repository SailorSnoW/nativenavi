// The shader library ABI of the offline build (through dm/gfx/tools/dmsh_abi.py) and the host. dmsh_abi.py reads only comment lines, #pragma once and
// "#define DMSH_<NAME> <hex, decimal or "string">" with an optional // comment. Part of the chain: after a change run dmsh_chain.py --header and rebuild the shaders.
// The translator (Program.cs) keeps its own copy of the c2 layout; test_dmsh_abi.py checks they agree.
#pragma once

// NVN's driver constant segments c[0] and c[2]: one layout in every game; the host fills one buffer per segment a shader reads
#define DMSH_DRV_SIZE 0xA00              // bytes

// c2: handle tables, a 32-bit word every DMSH_C2_ROW bytes, slot r at table + 8 r
#define DMSH_C2_ROW 8
#define DMSH_C2_TEX 0x20                 // combined handles; slots past 31 share their words with the image table
#define DMSH_C2_TEX_SLOTS 41
#define DMSH_C2_IMG 0x120
#define DMSH_C2_IMG_SLOTS 8
#define DMSH_C2_SEPTEX 0x168
#define DMSH_C2_SAMP 0x568
#define DMSH_C2_SEP_SLOTS 128
#define DMSH_SEPTEX_SLOT0 128            // separate texture r is texture slot 128 + r (NVN_SEPTEX_SLOT0)

// c0: tables of DMSH_C0_ROW-byte entries (64-bit address, 32-bit size) and single words
#define DMSH_C0_ROW 0x10
#define DMSH_C0_SSBO_SLOTS 16
#define DMSH_C0_SSBO_VS 0x110
#define DMSH_C0_SSBO_CS 0x310
#define DMSH_C0_SSBO_PS 0x510
#define DMSH_C0_SSBO_TS 0x670
#define DMSH_C0_SSBO_MS 0x770
#define DMSH_MESH_DRAW_SLOT 29           // the draw's {count, first} workgroups (DrawMeshTasksIndirect's record)
#define DMSH_MESH_GRID 1024              // Metal's limit of threadgroups in a mesh grid: a task-less mesh shader launches in blocks of this many
// A mesh stage array of 6000-16384 bytes lives in a slice of the buffer at this mesh slot (it shares the vertex argument table: vertex draws rebind what they read).
// Head words: _TICKET counter, _MASK slices - 1, _TAG >= the fence of each submission drawing a spilled mesh, _DONE the last completed fence, _OVER groups with no slice; from _FLAGS
// a word a slice, 0 free or the owner's tag. Lane 0 claims slice ticket & mask by CAS when its flag is 0 or a tag <= _DONE, up to _TRIES tickets, and the group clears it after its
// last read behind a device fence. No slice: _OVER + 1, nothing drawn. Tags compare by signed 32-bit difference
#define DMSH_MESH_SCRATCH_SLOT 7
#define DMSH_MESH_SCRATCH_TICKET 0
#define DMSH_MESH_SCRATCH_MASK 1
#define DMSH_MESH_SCRATCH_TAG 2
#define DMSH_MESH_SCRATCH_DONE 3
#define DMSH_MESH_SCRATCH_OVER 4
#define DMSH_MESH_SCRATCH_FLAGS 64
#define DMSH_MESH_SCRATCH_SLICES 16384
#define DMSH_MESH_SCRATCH_HEAD 65600
#define DMSH_MESH_SCRATCH_STRIDE 16384
#define DMSH_MESH_SCRATCH_TRIES 64
#define DMSH_C0_DISPATCH 0x200           // compute: workgroup counts x, y, z (u32 each)
#define DMSH_C0_DISPATCH_NVN2 4          // NVN2 compute's, below its per-shader layout at 0x10
#define DMSH_C0_CS_UBO 0x210             // compute: uniform slot r's {address, size} at 0x210 + 0x10 r, read through global memory
#define DMSH_C0_CS_UBO_SLOTS 16
// NVN2 graphics stage i (vertex, hull or mesh, domain, geometry, pixel) reads its pushed buffer entries as c[25 + i]: push block 1 + i of c[24]
#define DMSH_NVN2_STAGE_BANK 25

// NVN2 push constants as c[24] (NVN2 compute's c0 push region maps there); one block for graphics, one for compute
#define DMSH_PUSH_BANK 24
#define DMSH_PUSH_SIZE 0x100             // bytes

// Uniform bounds: a read past a slot's bound size gives 0, as on Maxwell. The host pads fixed-offset reads; computed ones compare with this bank's sizes
#define DMSH_CBSIZE_BANK 25
#define DMSH_CBSIZE_SLOTS 14             // u32 each, as uint4s
// Buffer texture offsets: Metal makes buffer textures only at aligned offsets, so the host widens the view down to the boundary and this bank gives each one's element bias,
// added to every coordinate. Wrapped 1D fetches have each logical width after their lead entries (0: an ordinary 1D texture)
#define DMSH_TBIAS_BANK 26
#define DMSH_TBIAS_SLOTS 512             // u32 each, as uint4s (a binding's index: its "tbx")
// Storage bounds: SBUFU / SBUFQ buffers have no size (on the console unmapped pages read 0): past the bound memory a load gives 0, a store or atomic is dropped.
// Storage slot s has two u32 at 2s, 2s + 1: the words readable before the address B, then B plus the words from it on
#define DMSH_SBLEN_BANK 27
#define DMSH_SBLEN_SLOTS 32              // two u32 each, as uint4s
// The push's word before the offset names a block, at b * DMSH_PUSH_SIZE in c[24]. Block 0: constants; block 1 + i: stage i's 16-byte {address, size} entries or 8-byte
// handles. Compute reads block 1 where its c0 layout counts it (control word +0xaa)
#define DMSH_PUSH_BLOCKS 8
#define DMSH_C0_POINT_SIZE 0x610         // GLSLC clamps a written gl_PointSize to it
#define DMSH_C0_SAMPLE_TABLE 0x620       // GLSLC's sample positions, a byte per (grid pixel, sample): x, y in 1/16 pixel, low / high nibble
#define DMSH_C0_SAMPLE_BYTES 16
#define DMSH_C0_SAMPLE_INFO 0x630        // sample index mask, log2 sample count, grid x mask (also y's shift), grid y mask
#define DMSH_C0_BASE_INSTANCE 0x644      // GLSLC adds it to Maxwell's instance ID

// Binding kinds: "kind" in a shader's <key>_b.json, mapped to NVN_RB_* by nvn_refl.m
#define DMSH_KIND_CBUF "cbuf"            // c[3 + reg]; fixed: bytes read at fixed offsets
#define DMSH_KIND_C1 "c1"
#define DMSH_KIND_DRVCB "drvcb"          // c[bank], bank 0 or 2
#define DMSH_KIND_CBSIZE "cbsize"
#define DMSH_KIND_TBIAS "tbias"
#define DMSH_KIND_SBLEN "sblen"
#define DMSH_KIND_SUPPORT "support"
#define DMSH_KIND_SBUF "sbuf"            // address in the stage's c0 table; rw: written
#define DMSH_KIND_SBUF_UBO "sbuf_ubo"    // address in uniform slot reg at byte off
#define DMSH_KIND_SBUF_PUSH "sbuf_push"  // address pushed, c[24] at byte off. sbx: length table slot, if bounded
#define DMSH_KIND_UBO_PTR "ubo_ptr"      // compute: read through its address in c0
#define DMSH_KIND_TEX "tex"              // combined, or DMSH_SEPTEX_SLOT0 + r for a separate texture
#define DMSH_KIND_TBUF "tbuf"
#define DMSH_KIND_IMG "img"              // atomic: image atomics
#define DMSH_KIND_SAMP "samp"
#define DMSH_KIND_SAMP_TEX "samp_tex"    // the sampler of texture slot reg's combined handle
#define DMSH_KIND_SAMP_ID "samp_id"      // constant sampler ID (reg)
#define DMSH_KIND_TEX_UBO "tex_ubo"      // bindless *_ubo: the handle in uniform slot reg at byte off
#define DMSH_KIND_SAMP_UBO "samp_ubo"
#define DMSH_KIND_IMG_UBO "img_ubo"
#define DMSH_KIND_TBUF_UBO "tbuf_ubo"
#define DMSH_SREG_C2 255                 // sreg value: the sampler word is c2's at byte soff
#define DMSH_KIND_TEX_C2A "tex_c2a"      // c2 handles indexed at run time, element k at off + 8 k
#define DMSH_KIND_SAMP_C2A "samp_c2a"
#define DMSH_KIND_TEX_POOL "tex_pool"    // the texture pool indexed by the handle's ID (a handle traced to no slot); elem: element type
#define DMSH_KIND_SAMP_POOL "samp_pool"
#define DMSH_KIND_TEX_HTAB "tex_htab"    // a host-built pool of the handles in uniform slot reg from byte off (element k at off + 8k)
#define DMSH_KIND_SAMP_HTAB "samp_htab"  // soff, sreg as SAMP_UBO's
#define DMSH_KIND_BORDER "border"        // a mask of the samplers to correct, then their factors

#define DMSH_KIND_GMEM_MAP "gmem_map"    // pool table for global memory traced to no storage buffer

// Raw global memory: the table is a storage buffer under a slot no game has; the shader binary-searches pool starts and adds the pool's delta
#define DMSH_GMEM_MAP_CB_SLOT 0xFF
#define DMSH_GMEM_MAP_ENTRIES 8192       // sorted by start; entry 0 empty, unused entries start at ~0
#define DMSH_GMEM_MAP_ENTRY 32           // start, end (exclusive), delta (Metal minus guest address), 64 bits each; u32 lookup hint; the byte span within one 4 GiB window, else 0
#define DMSH_GMEM_HINT_SHIFT 21          // 2 MiB chunks: entry (chunk & (entries - 1))'s hint is chunk << 13 | entry index, 0 = none
// Stores and atomics also set their entry's bit (bit i in word i / 32) in a write log at slot offset 1: the host learns the pools from it
#define DMSH_KIND_GMEM_LOG "gmem_log"    // a fresh log for each draw or dispatch
#define DMSH_GMEM_LOG_CB_OFFSET 1
// [storage slot, byte offset] pointer sources the host reads before each binding to admit their pools to the table
#define DMSH_GPTR_MAX 64

// A pool binding: u32 bound n (highest readable index), then one 8-byte resource ID per ID from DMSH_POOL_HEAD
#define DMSH_POOL_HEAD 16                // bytes: n, 4 padding, the read bitmap's GPU address (bit i: ID i read; 0 for handle tables)
#define DMSH_POOL_BIAS 4                 // BUFFER tables: byte offset of n+1 u32 leads after the IDs; 0 for others
#define DMSH_KIND_UNKNOWN "?"            // the host refuses the shader

// Alpha test state of _a entry points: uint function (NVN_ALPHA_FUNC_*), float reference
#define DMSH_ALPHA_BUFFER 30             // the fragment table's last binding; translated shaders use only buffer 0

// NVIDIA's zero-times-anything blend option (on in NVN's driver): DMSH_BLEND_ZERO_BITS an output, 0 unknown, 1 source weight 0 where alpha is 0, 2 where it is 1 (Apple's
// blender already drops ZERO factors). Second uint: DMSH_BLEND_ROUND_BITS an output, NVIDIA's rounding into a normalized target (DMSH_ROUND_*), 0 = Apple's conversion
#define DMSH_BLEND_BUFFER 28
#define DMSH_BLEND_ZERO_BITS 2
#define DMSH_BLEND_ROUND_BITS 2
#define DMSH_ROUND_8 1                   // 255
#define DMSH_ROUND_16 2                  // 65535
#define DMSH_ROUND_10_2 3                // 1023, 3

// Border colours: a twin <member>_w per sampler member; per sample r + (twin - r) * k when its bit is set (nvn_border.h)
#define DMSH_BORDER_SAMPLERS 32          // bits of the mask word at byte 0; later members keep Metal's nearest colour
#define DMSH_BORDER_K 0x10               // bit i's factors: a float4 at DMSH_BORDER_K + 16 i
#define DMSH_BORDER_TWIN "_w"
#define DMSH_BORDER_MEMBER "dm_border"
// bool, every stage: true drops the corrections (no twin sampler bound) and their registers; undefined: every sample corrects by its bit
#define DMSH_BORDER_FC 216
// bool, pixel shaders with dm_blend0: true when both blend words are 0; false or undefined reads DMSH_BLEND_BUFFER (pair archives keep false)
#define DMSH_BLEND_OFF_FC 217
// bool, every stage but vertex: KosmicKrisp's KK_WORKAROUND_3 ballot rewrite is used (dmsh_metalwa.py; undefined: false). The host and pso_offline.py leave it false (issue #3405)
#define DMSH_MWA_FC 220
// Logic op state of _l entry points: uint op (NVN_LOGIC_OP_*), 12 bytes padding, a uint4 channel mask per colour target
#define DMSH_LOGIC_BUFFER 29
// bool at + n: colour target n is attached
#define DMSH_LOGIC_FC 200
// Typed-output entry point main0_u: bool, output n goes to an unsigned (DMSH_UINT_FC + n) or signed (DMSH_SINT_FC + n) target, else float; bools: alpha test, logic op
#define DMSH_UINT_FC 208
#define DMSH_SINT_FC 224
#define DMSH_TYPED_ALPHA_FC 232
#define DMSH_TYPED_LOGIC_FC 233
// Threadgroup memory past DMSH_TG_MAX: a device buffer at this compute index, a tg_spill-byte slice per workgroup at its index in the grid
#define DMSH_TG_SLOT 30
#define DMSH_TG_MAX 0x8000
// state: the submission's GPU abort address (u64), the capped loop site (atomic u32, UINT32_MAX); only a kernel over its own budget records one
#define DMSH_LOOP_SLOT 28
#define DMSH_LOOP_BYTES 16
#define DMSH_LOOP_HIT 8
// Two-pass compute: DMSH_SPLIT_FC undefined for one pass, false for pass 0 (writes its u32s at DMSH_SPLIT_SLOT by thread index in the grid), true for pass 1; a barrier between
#define DMSH_SPLIT_FC 218
#define DMSH_SPLIT_SLOT 29
