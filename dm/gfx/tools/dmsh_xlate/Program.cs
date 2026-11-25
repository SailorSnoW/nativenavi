// Switch shaders → SPIR-V (the Ryujinx.Graphics.Shader translator) for the offline metallib build, the translator's queries answered from the job file.
// Depth is not rewritten (NVN's near = 0): dmsh_build.py adds the GL convention as a second entry point.
// Usage: dmsh_xlate <job.json> <output dir>
//   Job: [{ "key", "stage", "code" (SPH + instructions), "arch": "sm86" (NVN2), "tex": { slot: type }, "img": { slot: { "type", "fmt" } }, "attr", "fout", "local" }]
//   Output: <key>.spv and <key>.res.json
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static partial class SwshXlate
{
    // NVN's c[2] handle tables: byte offsets, 8 bytes a slot
    const int TexBase = 0x20, ImgBase = 0x120, ImgSlots = 8, SepTexBase = 0x168, SepTexSlot0 = 128, SampBase = 0x568;
    // one element per c[2] handle word up to the separate sampler table's end, so any word a computed index reaches is in the array
    const int C2HandleWords = (SampBase + 8 * 128) / 8;

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code;
        readonly JsonObject _job;
        int _binding = 1;
        public readonly List<string> Notes = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job, Dictionary<int, SamplerType> imageTypes = null, Dictionary<int, SamplerType> textureTypes = null)
        {
            if (job["rt"] != null) _binding = RayTracingProgram.ReservedBindings;
            else if (job["geometry_topology"] != null) _binding = 40;
            _code = code;
            _job = job;
            _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>();
            _imageTypes = imageTypes;
            _textureTypes = textureTypes;
        }
        public bool QueryRawGlobalMemory() => _job["rt"] != null;
        public bool QueryHostSupportsImageLoadFormatted() => _job["rt"] == null;
        // A TXQ on an image handle asks a type no table answers: the first translation records it (ImageQueryGuessed), and Main translates again with the types
        // the surface instructions gave (ImageTypesOf)
        readonly Dictionary<int, SamplerType> _imageTypes;
        public bool ImageQueryGuessed;
        public Dictionary<int, SamplerType> ImageTypesOf(ShaderProgram p)
        {
            var types = new Dictionary<int, SamplerType>();
            foreach (var im in p.Info.Images) if (im.CbufSlot == (_job["rt"] != null ? 3 : 2) || im.CbufSlot < 0) types[im.HandleIndex] = im.Type & (SamplerType.Mask | SamplerType.Array);
            return types;
        }
        // The same for a texture, by texture slot (a query's handle has another sampler word, so it gets a binding of its own); a slot typed two ways is left out
        readonly Dictionary<int, SamplerType> _textureTypes;
        readonly HashSet<int> _guessedHandles = new();
        public Dictionary<int, SamplerType> TextureTypesOf(ShaderProgram p)
        {
            var types = new Dictionary<int, SamplerType>();
            var mixed = new HashSet<int>();
            foreach (var t in p.Info.Textures)
            {
                if (t.ArrayLength != 1) continue;
                SamplerType ty = t.Type & (SamplerType.Mask | SamplerType.Array | SamplerType.Multisample);
                int slot = Slot(t.HandleIndex, t.CbufSlot, false);
                if (slot < 0 || (ty == SamplerType.Texture2D && _guessedHandles.Contains(t.HandleIndex))) continue;
                if (types.TryGetValue(slot, out SamplerType seen) && seen != ty) mixed.Add(slot); else types[slot] = ty;
            }
            var guessed = new HashSet<int>(_guessedHandles.Select(h => Slot(h, -1, false)));
            return types.Where(kv => !mixed.Contains(kv.Key) && guessed.Contains(kv.Key) && kv.Value != SamplerType.Texture2D).ToDictionary(kv => kv.Key, kv => kv.Value);
        }
        // c1 holds the jump tables of indirect jumps (BRX)
        public int QueryConstantBuffer1Size() => _c1.Length;
        public int[] QueryAmpereComputeC0Layout() => _job["nvn2_cs"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;
        public int QueryAmpereStageBank() => _job["stage_bank"] is JsonNode b ? (int)b : -1;
        public int[] QueryAmpereStageMemory() => _job["stage_memory"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;
        public uint ConstantBuffer1Read(int offset)
        {
            if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset);
            Notes.Add($"c1 read out of bounds +{offset:X} (c1 length {_c1.Length})");
            return 0;
        }
        // All resources in set 0, each a distinct binding: spirv-cross emits them as one argument buffer
        public void Log(string message) => Notes.Add("translator: " + message);
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        // An address past the code is no code: FindBrxTargetsAmpere ends a jump table on it (a slice there would throw)
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => address >= (ulong)_code.Length ? default : MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        // Handles traced to no slot index the pools by their IDs (20 and 12 bits); dmsh_build.py makes them bounded pointers, so the lengths are the whole ID ranges
        public int QuerySamplerArrayLengthFromPool() => 1 << 12;
        // c[2]: every handle word; a uniform buffer: as many 8-byte handles as 64 KB holds. Both become pointers to tables the replay builds, with a run-time bound
        public int QueryTextureArrayLengthFromBuffer(int slot) => slot == 2 ? C2HandleWords : 8192;
        public int QueryTextureArrayLengthFromPool() => 1 << 20;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        // Numbered as dmsh_reflect.py decode_tex numbers the reflection's; the handle ORs two c[2] words in no fixed order, so both are tried; -1 when neither is in the table
        int Slot(int handle, int cbufSlot, bool image)
        {
            bool rt = _job["rt"] != null;
            if (cbufSlot >= 0 && (cbufSlot & 0xffff) != (rt ? 3 : 2)) return -1;
            (int w0, int w1, TextureHandleType ty) = TextureHandle.UnpackOffsets(handle);
            foreach (int w in ty == TextureHandleType.CombinedSampler || ty == TextureHandleType.SeparateConstantSamplerHandle ? new[] { w0 } : new[] { w0, w1 })
            {
                int b = w * 4 - (rt ? (int)_job["rt"]["descriptor_offset"] + 0x430 : 0);
                if (b % 8 != 0) continue;
                if (image && b >= ImgBase && b < ImgBase + 8 * ImgSlots) return (b - ImgBase) / 8;
                if (!image && b >= TexBase && b < SepTexBase) return (b - TexBase) / 8;
                if (!image && b >= SepTexBase && b < SampBase) return SepTexSlot0 + (b - SepTexBase) / 8;
            }
            return -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, false), islot = Slot(handle, cbufSlot, true);
            string t = (slot >= 0 ? Get("tex", slot) : null) ?? (islot >= 0 ? Get("img", islot) : null);
            if (t == null && islot >= 0 && cbufSlot < 0)
            {
                if (_imageTypes != null && _imageTypes.TryGetValue(handle, out SamplerType it)) { Notes.Add($"texture handle {handle:X} is image slot {islot}: type {it} from the shader's image instructions"); return it; }
                ImageQueryGuessed = true;
            }
            if (t == null && slot >= 0 && islot < 0)
            {
                if (_textureTypes != null && _textureTypes.TryGetValue(slot, out SamplerType tt)) { Notes.Add($"texture handle {handle:X} is texture slot {slot}: type {tt} from the shader's other instructions"); return tt; }
                _guessedHandles.Add(handle);
            }
            if (t == null) { Notes.Add($"texture handle {handle:X} (constant buffer {cbufSlot}, slot {slot}) has no type in the answer table, using 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        // Slots of a 1D integer fetch made a wrapped 2D read (x & 0x3FFF, x >> 14): a 1D texture or a texture buffer, which only the bound descriptor tells
        public readonly SortedSet<int> Fetch1D = new();
        public void RecordTexelFetch1D(int handle)
        {
            int slot = Slot(handle, -1, false);
            if (slot >= 0) Fetch1D.Add(slot);
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, true);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (_job["rt"] != null && (f == null || !Enum.TryParse(f, true, out TextureFormat _)))
                throw new InvalidOperationException($"RT image handle {handle:X} in bank {cbufSlot} has no verified format answer");
            if (f == null) { Notes.Add($"image handle {handle:X} slot {slot} has no format in the answer table, using R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public bool QueryGeometryInputFixed() => _job["geometry_topology"] != null;
        // Metal cannot supply the native NV geometry-passthrough stage afterwards
        public bool QueryHostSupportsGeometryShaderPassthrough() => _job["geometry_topology"] == null;
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default;
            Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            bool earlyZ = _job["early_fragment_tests"] is JsonNode ez && (bool)ez;   // control +0x5b
            return new GpuGraphicsState(earlyZ, _job["geometry_topology"] is JsonNode gt ? (InputTopology)(int)gt : InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false /* depth not rewritten */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* no Y flip */, true /* origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        public bool QueryHostSupportsDepthClipControl() => false;   // the depth convention is added per entry point in the MSL
        // reduced: relaxed precision, NoContraction only where exact (the default marks every operation)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // softfloat64 only on the retry of a shader that declared a 64-bit float (Metal has no double): answering no for all would drop Float64 from the others
        public bool QueryHostSupportsShaderFloat64() => !(_job["softfloat64"] is JsonNode n && (bool)n);
    }

    // SPIRV-Cross then writes double, which Metal rejects
    static bool UsesFloat64(byte[] spirv)
    {
        for (int i = 20; i + 12 <= spirv.Length; )
        {
            uint word = BitConverter.ToUInt32(spirv, i);
            if ((word & 0xffff) == 22 && (word >> 16) == 3 && BitConverter.ToUInt32(spirv, i + 8) == 64) return true;
            i += Math.Max(1, (int)(word >> 16)) * 4;
        }
        return false;
    }

    static RayTracingProgram ReadRayTracingProgram(JsonObject spec)
    {
        if (spec == null) return null;
        var entries = spec["entries"].AsArray().Select(node => new RayTracingEntry(
            (ulong)node["address"], (int)node["id"], (int)node["owner"], (RayTracingStage)(int)node["kind"], (int)node["frame"])).ToArray();
        var modules = spec["modules"].AsArray().Select(node => new RayTracingModuleRange((ulong)node["address"], (int)node["length"])).ToArray();
        var moves = spec["moves"].AsArray().Select(node => new RayTracingLogicalMove((ulong)node["address"], (ulong)node["template"], (int)node["id"])).ToArray();
        return new RayTracingProgram(entries, modules, moves, (int)spec["core_offset"], (int)spec["descriptor_offset"]);
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // the digest of its sources, stamped at build time (dmsh_chain.py --xlate-source-sha, native_tools.sh)
        {
            Console.WriteLine(SourceSha);
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray();
        string outDir = args[1];
        Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                var acc = new Accessor(code, job);
                // debug = false: DebugMode (which skips the AST optimizer) off
                var flags = TranslationFlags.MetalImageAtomics |
                    (job["rt"] != null ? 0 : TranslationFlags.MetalUniformBounds | TranslationFlags.MetalBufferImageBias | TranslationFlags.MetalStorageBounds) |
                    (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) |
                    ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0) |
                    ((string)job["arch"] == "sm86" ? TranslationFlags.Ampere : 0);
                RayTracingProgram rt = ReadRayTracingProgram(job["rt"] as JsonObject);
                TranslatorContext Context(Accessor accessor) => rt == null
                    ? Translator.CreateContext(0, accessor, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags))
                    : Translator.CreateRayTracingContext(accessor, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags), rt);
                var ctx = Context(acc);
                bool geometry = job["geometry_topology"] != null;
                ShaderProgram p = ctx.Translate(geometry);
                Dictionary<int, SamplerType> imageTypes = null, textureTypes = null;
                if (acc.ImageQueryGuessed && (imageTypes = acc.ImageTypesOf(p)).Count == 0) imageTypes = null;   // again with the images' types
                if ((textureTypes = acc.TextureTypesOf(p)).Count == 0) textureTypes = null;   // and with the textures' types
                if (imageTypes != null || textureTypes != null)
                {
                    acc = new Accessor(code, job, imageTypes, textureTypes);
                    ctx = Context(acc);
                    p = ctx.Translate(geometry);
                }
                if (UsesFloat64(p.BinaryCode))
                {
                    job["softfloat64"] = true;
                    acc = new Accessor(code, job, imageTypes, textureTypes);
                    ctx = Context(acc);
                    p = ctx.Translate(geometry);
                    acc.Notes.Add("FP64 lowered to FP32 (DoubleToFloat)");
                }
                if (geometry)
                {
                    // A fresh context: translation mutates the decoded program.
                    var reader = Translator.CreateContext(0, new Accessor(code, job, imageTypes, textureTypes), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                    File.WriteAllBytes(Path.Combine(outDir, key + ".reader.spv"), reader.GenerateVertexPassthroughForCompute().BinaryCode);
                }
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["geometry_stride"] = geometry ? ctx.GetResourceReservations().OutputSizePerInvocation : 0,
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                };
                if (rt != null) res["rt"] = new JsonObject
                {
                    ["version"] = 1, ["scene_binding"] = RayTracingProgram.SceneBinding,
                    ["parameters_binding"] = RayTracingProgram.ParametersBinding, ["errors_binding"] = RayTracingProgram.ErrorsBinding,
                    ["instance_sbt_binding"] = RayTracingProgram.InstanceSbtBinding,
                    ["parameter_words"] = (RayTracingProgram.MembershipBase + (rt.Entries.Max(e => e.LogicalId) / 32 + 1) + 3) & ~3,
                    ["image_class_base"] = RayTracingProgram.ImageClassBase, ["membership_base"] = RayTracingProgram.MembershipBase,
                    ["local_bytes"] = rt.LocalMemoryBytes, ["local_bias"] = rt.LocalMemoryBias,
                    ["core_offset"] = rt.CoreOffset, ["descriptor_offset"] = rt.DescriptorOffset,
                    ["entries"] = job["rt"]["entries"].DeepClone(),
                };
                if (rt != null)
                {
                    var slots = new JsonObject();
                    foreach (var pair in ctx.GetRayTracingAccelerationSlots())
                        slots[pair.Key.ToString()] = new JsonArray(pair.Value.Select(slot => (JsonNode)JsonValue.Create(slot)).ToArray());
                    res["rt"]["as_slots_by_module"] = slots;
                }
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["fixed"] = b.FixedSize });   // fixed: bytes read at fixed offsets, which the host makes readable
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });   // flags: 1 written, 2 bounded by the length table
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list)
                    {
                        var o = new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags };
                        if (t.BufferBias >= 0) o["tbx"] = t.BufferBias;
                        if (t.BufferBias >= 0 && (t.Type & SamplerType.Mask) == SamplerType.Texture2D) o["row_bias"] = true;
                        res[name].AsArray().Add((JsonNode)o);
                    }
                if (acc.Fetch1D.Count > 0)
                {
                    var f1 = new JsonArray();
                    foreach (int h in acc.Fetch1D) f1.Add((JsonNode)JsonValue.Create(h));
                    res["fetch1d"] = f1;
                }
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                ok++;
            }
            catch (Exception e)
            {
                bad++;
                Console.Error.WriteLine($"[RED] {key}: {e.GetType().Name}: {e.Message.Split('\n')[0]}");
                if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace);
            }
        }
        Console.WriteLine($"translated: {ok} ok, {bad} failed");
        return bad == 0 ? 0 : 1;
    }
}
