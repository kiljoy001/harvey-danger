#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/compiler/pan_nir.h"
#include "panfrost/lib/pan_shader.h"
#include "panfrost/compiler/bifrost/bifrost/disassemble.h"
#include "spirv/nir_spirv.h"
#include "glsl_types.h"
#include "panfrost/lib/pan_format.h"

#include "util/u_dynarray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Set when the SPIR-V shader contains a texture instruction. */
static bool spv_has_texture;

/*
 * On Bifrost (arch <= 7) the RENDERER_STATE descriptor *is* the shader
 * program descriptor: the blob pointer, register allocation, preload and
 * the message pair all live in it, and the job slot picks the stage.  So a
 * draw needs one packed RENDERER_STATE per stage and nothing else from
 * Mesa's descriptor set.
 */
static void
dump_rsd(const char *name, const struct pan_shader_info *info,
         uint64_t shader_placeholder)
{
   struct mali_renderer_state_packed rsd;

   memset(&rsd, 0, sizeof rsd);
   pan_cast_and_pack(&rsd, RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(info, shader_placeholder, &cfg);

      if(info->stage == MESA_SHADER_FRAGMENT){
         /* On Bifrost the blend equation is a separate BLEND descriptor
          * referenced by the draw, not part of the renderer state.  The
          * rest is what Gallium would merge in for an opaque draw: with
          * a zeroed descriptor the depth function decodes as Never and
          * every fragment is killed. */
         cfg.multisample_misc.sample_mask = 0xffff;
         cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
         cfg.multisample_misc.overdraw_alpha0 = true;
         cfg.multisample_misc.overdraw_alpha1 = true;
         cfg.multisample_misc.fixed_function_depth_range_fixed = true;
         cfg.multisample_misc.shader_depth_range_fixed = true;
         cfg.multisample_misc.fixed_function_near_discard = true;
         cfg.multisample_misc.fixed_function_far_discard = true;
      } else {
         /* The tiler reads the transformed position out of the varying
          * buffer, so the shader has to be handed its address. */
         cfg.preload.vertex.position_result_address_lo = true;
         cfg.preload.vertex.position_result_address_hi = true;
      }
   }

   char path[64];
   snprintf(path, sizeof path, "%s.rsd", name);
   FILE *f = fopen(path, "wb");
   if(f != NULL){
      fwrite(&rsd, 1, sizeof rsd, f);
      fclose(f);
   }

   printf("%s: rsd %u bytes shader %#llx attr %u varying %u ubo %u "
          "tex %u samp %u work_regs %u uniform_preload %u\n",
          name, (unsigned)sizeof rsd,
          (unsigned long long)shader_placeholder,
          info->attribute_count, info->varyings.formats.count,
          info->ubo_count, info->texture_count, info->sampler_count,
          info->work_reg_count, info->push.count);
}

static void
dump(const char *name, nir_shader *nir, struct pan_varying_layout *layout)
{
   struct pan_compile_inputs inputs = {0};
   inputs.gpu_id = 0x74021000ull;      /* G52, raw GPU_ID */
   inputs.gpu_variant = 0;
   inputs.varying_layout = layout;
   inputs.no_idvs = false;

   struct util_dynarray bin;
   util_dynarray_init(&bin, nir);
   struct pan_shader_info info;
   pan_shader_compile(nir, &inputs, &bin, &info);
   /* The compiler counts textures from API bindings; SPIR-V has none, so
    * restore what we know about this shader. */
   if(spv_has_texture && info.stage == MESA_SHADER_FRAGMENT){
      info.texture_count = 1;
      info.sampler_count = 1;
   }

   char path[64];
   snprintf(path, sizeof path, "%s.bin", name);
   FILE *f = fopen(path, "wb");
   if(f != NULL){
      fwrite(bin.data, 1, bin.size, f);
      fclose(f);
   }
   printf("%s: binary %u bytes stage %u tls %llu outputs %#llx\n",
          name, bin.size, (unsigned)info.stage,
          (unsigned long long)info.tls_size,
          (unsigned long long)info.outputs_written);
   printf("--- %s ---\n", name);
   disassemble_bifrost(stdout, bin.data, bin.size, false);
   printf("--- end %s ---\n", name);

   dump_rsd(name, &info, 0xDEADB000ull);
}

static void
emit_vertex(const char *name, float sx, float sy)
{
   nir_builder vb = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
                                                   &bifrost_nir_options_v6,
                                                   "gpucc-vs");
   nir_def *pos = nir_load_input(&vb, 4, 32, nir_imm_int(&vb, 0),
                                 .base = 0,
                                 .io_semantics.location = VERT_ATTRIB_GENERIC0,
                                 .io_semantics.num_slots = 1);
   nir_def *xscale = nir_imm_float(&vb, sx);
   nir_def *yscale = nir_imm_float(&vb, sy);
   nir_def *half = nir_imm_float(&vb, 0.5f);
   nir_def *px = nir_fadd(&vb, nir_fmul(&vb, nir_channel(&vb, pos, 0), xscale), xscale);
   nir_def *py = nir_fadd(&vb, nir_fmul(&vb, nir_channel(&vb, pos, 1), yscale), yscale);
   nir_def *pz = nir_fadd(&vb, nir_fmul(&vb, nir_channel(&vb, pos, 2), half), half);
   nir_def *out = nir_vec4(&vb, px, py, pz, nir_channel(&vb, pos, 3));
   nir_store_output(&vb, out, nir_imm_int(&vb, 0),
                    .base = 0, .range = 1, .write_mask = 0xf,
                    .src_type = nir_type_float32,
                    .io_semantics.location = VARYING_SLOT_POS,
                    .io_semantics.num_slots = 1);
   vb.shader->info.outputs_written |= 1ull << VARYING_SLOT_POS;
   vb.shader->info.inputs_read |= 1ull << VERT_ATTRIB_GENERIC0;
   nir_validate_shader(vb.shader, name);
   struct pan_varying_layout layout = {0};
   pan_varying_collect_formats(&layout, vb.shader, 0x74021000ull, true, false);
   pan_build_varying_layout_compact(&layout, vb.shader, 0x74021000ull);
   dump(name, vb.shader, &layout);
}


/* Bifrost wants a constant texture/sampler index per tex instruction, not
 * derefs.  We have exactly one texture bound at index 0. */
static bool
lower_sampler_derefs(nir_builder *b, nir_instr *instr, void *data)
{
   nir_tex_instr *tex;
   bool changed = false;
   int i;

   if(instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   for(i = tex->num_srcs - 1; i >= 0; i--){
      if(tex->src[i].src_type == nir_tex_src_texture_deref ||
         tex->src[i].src_type == nir_tex_src_sampler_deref){
         nir_tex_instr_remove_src(tex, i);
         changed = true;
      }
   }
   tex->texture_index = 0;
   tex->sampler_index = 0;
   return changed;
}

/* The GLSL path leaves gl_Position's viewport transform to the driver as
 * load_viewport_scale/offset sysvals.  We bake it, like panvk substitutes
 * its pipeline state, because gpufs has no per-draw uniforms yet. */
static bool
lower_viewport_sysvals(nir_builder *b, nir_instr *instr, void *data)
{
   const float *v = data;      /* scale x, scale y, offset x, offset y */
   nir_intrinsic_instr *intr;
   nir_def *val;

   if(instr->type != nir_instr_type_intrinsic)
      return false;
   intr = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(instr);

   switch(intr->intrinsic){
   case nir_intrinsic_load_viewport_scale:
      val = nir_imm_vec4(b, v[0], v[1], 0.5f, 1.0f);
      break;
   case nir_intrinsic_load_viewport_offset:
      val = nir_imm_vec4(b, v[2], v[3], 0.5f, 0.0f);
      break;
   default:
      return false;
   }
   nir_def_replace(&intr->def, val);
   return true;
}

/* A linear RGBA8 2D texture, its surface, and a nearest/clamp sampler.
 * The pointers are placeholders: gpufs patches them at submit time. */
static void
emit_texture(unsigned width, unsigned height)
{
   struct mali_texture_packed tex;
   struct mali_surface_with_stride_packed surf;
   struct mali_sampler_packed samp;

   memset(&tex, 0, sizeof tex);
   pan_pack(&tex, TEXTURE, cfg) {
      cfg.type = 2;
      cfg.dimension = MALI_TEXTURE_DIMENSION_2D;
      /* The Format field is packed: format << 12 | component order. */
      cfg.format = MALI_PACK_FMT(RGBA8_UNORM, RGBA, L);
      cfg.width = width;
      cfg.height = height;
      cfg.swizzle = 1672;	/* RGBA component swizzle */
      cfg.texel_ordering = MALI_TEXTURE_LAYOUT_LINEAR;
      cfg.levels = 1;
      cfg.sample_count = 1;
      cfg.surfaces = 0x1000;
      cfg.array_size = 1;
      cfg.depth = 1;
   }

   memset(&surf, 0, sizeof surf);
   pan_pack(&surf, SURFACE_WITH_STRIDE, cfg) {
      cfg.pointer = 0x2000;
      cfg.row_stride = width * 4;
      cfg.surface_stride = width * 4 * height;
   }

   memset(&samp, 0, sizeof samp);
   pan_pack(&samp, SAMPLER, cfg) {
      cfg.wrap_mode_r = MALI_WRAP_MODE_CLAMP_TO_EDGE;
      cfg.wrap_mode_s = MALI_WRAP_MODE_CLAMP_TO_EDGE;
      cfg.wrap_mode_t = MALI_WRAP_MODE_CLAMP_TO_EDGE;
      cfg.minify_nearest = true;
      cfg.magnify_nearest = true;
      cfg.mipmap_mode = MALI_MIPMAP_MODE_NONE;
      /* ulod is 8.8 fixed point: 0x1fff is 31.996, what Mesa writes. */
      cfg.minimum_lod = 0;
      cfg.maximum_lod = 0x1fff;
   }

   FILE *f;
   if((f = fopen("tex.bin", "wb")) != NULL){ fwrite(&tex, 1, sizeof tex, f); fclose(f); }
   if((f = fopen("surf.bin", "wb")) != NULL){ fwrite(&surf, 1, sizeof surf, f); fclose(f); }
   if((f = fopen("samp.bin", "wb")) != NULL){ fwrite(&samp, 1, sizeof samp, f); fclose(f); }
   printf("tex %u bytes surf %u bytes samp %u bytes\n",
          (unsigned)sizeof tex, (unsigned)sizeof surf, (unsigned)sizeof samp);
}

/* Unpack a framebuffer descriptor so its fields can be read back, used to
 * check the emitter against the capture. */
static void
dump_fb(const char *path)
{
   unsigned char buf[0x100];
   FILE *f = fopen(path, "rb");
   if(f == NULL){ printf("no %s\n", path); return; }
   if(fread(buf, 1, sizeof buf, f) != sizeof buf){ }
   fclose(f);

   pan_unpack((struct mali_framebuffer_parameters_packed *)buf,
             FRAMEBUFFER_PARAMETERS, p);
   printf("params: frames %d/%d/%d sample_locations %#lx dcds %#lx\n",
          p.pre_frame_0, p.pre_frame_1, p.post_frame,
          (unsigned long)p.sample_locations, (unsigned long)p.frame_shader_dcds);
   printf("  width %u height %u bound_min %u,%u bound_max %u,%u\n",
          p.width, p.height, p.bound_min_x, p.bound_min_y,
          p.bound_max_x, p.bound_max_y);
   printf("  samples %u pattern %d tiebreak %d tilesize %u xdown %u ydown %u\n",
          p.sample_count, p.sample_pattern, p.tie_break_rule,
          p.effective_tile_size, p.x_downsampling_scale,
          p.y_downsampling_scale);
   printf("  rt_count %u cballoc %u s_clear %u z_ifmt %d tiler %#lx\n",
          p.render_target_count, p.color_buffer_allocation, p.s_clear,
          p.z_internal_format, (unsigned long)p.tiler);

   pan_unpack((struct mali_render_target_packed *)(buf + pan_size(FRAMEBUFFER)),
             RENDER_TARGET, rt);
   printf("rt: internal %d write_enable %d writeback %d block %d msaa %d\n",
          rt.rgb.internal_format, rt.rgb.write_enable,
          rt.rgb.writeback_format, rt.rgb.writeback_block_format,
          rt.rgb.writeback_msaa);
   printf("  srgb %d dither %d swizzle %#x clean %d\n",
          rt.rgb.srgb, rt.rgb.dithering_enable, rt.rgb.swizzle,
          rt.rgb.clean_tile_write_enable);
   printf("  base %#lx stride %lu clear %08x %08x %08x %08x\n",
          (unsigned long)rt.rgb.writeback_buffer.base,
          (unsigned long)rt.rgb.writeback_buffer.row_stride,
          (unsigned)rt.rgb.clear.color_0, (unsigned)rt.rgb.clear.color_1,
          (unsigned)rt.rgb.clear.color_2, (unsigned)rt.rgb.clear.color_3);
}

/* The sample position table the hardware reads through the framebuffer
 * descriptor.  Each pattern gets 256 bytes: 32 (x,y) pairs in 8:8 fixed
 * point, then the origin, then padding.  The positions are the standard
 * GL/Vulkan sample locations. */
#define SAMP16(x, y) ((uint16_t)(((x) + 8) * 16)), ((uint16_t)(((y) + 8) * 16))
#define SAMP4(x, y)  SAMP16((x) * 4, (y) * 4)
#define SAMP8(x, y)  SAMP16((x) * 2, (y) * 2)

static void
put_sample(uint16_t *out, unsigned pattern, unsigned index, unsigned x,
           unsigned y)
{
   out[pattern * 128 + index * 2 + 0] = x;
   out[pattern * 128 + index * 2 + 1] = y;
}

static void
put_origin(uint16_t *out, unsigned pattern, unsigned x, unsigned y)
{
   out[pattern * 128 + 64 + 0] = x;
   out[pattern * 128 + 64 + 1] = y;
}

static void
emit_test_data(void)
{
   static uint16_t samples[6 * 128];
   float verts[12] = {
      -1.0f, -1.0f, 0.0f, 1.0f,
       3.0f, -1.0f, 0.0f, 1.0f,
      -1.0f,  3.0f, 0.0f, 1.0f,
   };
   FILE *f;

   memset(samples, 0, sizeof samples);

   /* Single sampled. */
   put_sample(samples, 0, 0, SAMP4(0, 0));
   put_origin(samples, 0, SAMP4(0, 0));

   /* Ordered 4x grid. */
   put_sample(samples, 1, 0, SAMP4(-1, -1));
   put_sample(samples, 1, 1, SAMP4( 1, -1));
   put_sample(samples, 1, 2, SAMP4(-1,  1));
   put_sample(samples, 1, 3, SAMP4( 1,  1));
   put_origin(samples, 1, SAMP4(0, 0));

   /* Rotated 4x grid. */
   put_sample(samples, 2, 0, SAMP8(-1, -3));
   put_sample(samples, 2, 1, SAMP8( 3, -1));
   put_sample(samples, 2, 2, SAMP8(-3,  1));
   put_sample(samples, 2, 3, SAMP8( 1,  3));
   put_origin(samples, 2, SAMP8(0, 0));

   /* D3D 8x grid. */
   put_sample(samples, 3, 0, SAMP16( 1, -3));
   put_sample(samples, 3, 1, SAMP16(-1,  3));
   put_sample(samples, 3, 2, SAMP16( 5,  1));
   put_sample(samples, 3, 3, SAMP16(-3, -5));
   put_sample(samples, 3, 4, SAMP16(-5,  5));
   put_sample(samples, 3, 5, SAMP16(-7, -1));
   put_sample(samples, 3, 6, SAMP16( 3,  7));
   put_sample(samples, 3, 7, SAMP16( 7, -7));
   put_origin(samples, 3, SAMP16(0, 0));

   /* D3D 16x grid. */
   put_sample(samples, 4,  0, SAMP16( 1,  1));
   put_sample(samples, 4,  1, SAMP16(-1, -3));
   put_sample(samples, 4,  2, SAMP16(-3,  2));
   put_sample(samples, 4,  3, SAMP16( 4, -1));
   put_sample(samples, 4,  4, SAMP16(-5, -2));
   put_sample(samples, 4,  5, SAMP16( 2,  5));
   put_sample(samples, 4,  6, SAMP16( 5,  3));
   put_sample(samples, 4,  7, SAMP16( 3, -5));
   put_sample(samples, 4,  8, SAMP16(-2,  6));
   put_sample(samples, 4,  9, SAMP16( 0, -7));
   put_sample(samples, 4, 10, SAMP16(-4, -6));
   put_sample(samples, 4, 11, SAMP16(-6,  4));
   put_sample(samples, 4, 12, SAMP16(-8,  0));
   put_sample(samples, 4, 13, SAMP16( 7, -4));
   put_sample(samples, 4, 14, SAMP16( 6,  7));
   put_sample(samples, 4, 15, SAMP16(-7, -8));
   put_origin(samples, 4, SAMP16(0, 0));

   /* Rotated 2x grid. */
   put_sample(samples, 5, 0, SAMP4( 1,  1));
   put_sample(samples, 5, 1, SAMP4(-1, -1));
   put_origin(samples, 5, SAMP4(0, 0));

   if((f = fopen("samples.bin", "wb")) != NULL){
      fwrite(samples, 1, sizeof samples, f);
      fclose(f);
   }
   if((f = fopen("verts.bin", "wb")) != NULL){
      fwrite(verts, 1, sizeof verts, f);
      fclose(f);
   }
   printf("test data samples %u verts %u\n", (unsigned)sizeof samples,
          (unsigned)sizeof verts);
}

/* The framebuffer descriptor: 128 bytes of parameters, then one
 * 64-byte render target. */
static void
emit_framebuffer(uint64_t rtbase, uint64_t tiler, uint64_t samples)
{
   struct mali_framebuffer_packed fbd;
   struct mali_render_target_packed rt;
   FILE *f;

   memset(&fbd, 0, sizeof fbd);
   pan_section_pack(&fbd, FRAMEBUFFER, PARAMETERS, p) {
      p.sample_locations = samples;
      p.width = 64;
      p.height = 64;
      p.bound_max_x = 63;
      p.bound_max_y = 63;
      p.sample_count = 1;
      p.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
      p.effective_tile_size = 256;
      p.render_target_count = 1;
      p.color_buffer_allocation = 1024;
      p.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;
      p.tiler = tiler;
   }
   pan_section_pack(&fbd, FRAMEBUFFER, PADDING, pad) { }

   memset(&rt, 0, sizeof rt);
   pan_cast_and_pack(&rt, RGB_RENDER_TARGET, cfg) {
      cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
      cfg.write_enable = true;
      cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
      cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
      cfg.dithering_enable = true;
      cfg.swizzle = 0x688;
      cfg.clean_tile_write_enable = true;
      cfg.writeback_buffer.base = rtbase;
      cfg.writeback_buffer.row_stride = 64 * 4;
      cfg.clear.color_0 = 0xffff0000;
      cfg.clear.color_1 = 0xffff0000;
      cfg.clear.color_2 = 0xffff0000;
      cfg.clear.color_3 = 0xffff0000;
   }

   if((f = fopen("fb_params.bin", "wb")) != NULL){ fwrite(&fbd, 1, sizeof fbd, f); fclose(f); }
   if((f = fopen("fb_rt.bin", "wb")) != NULL){ fwrite(&rt, 1, sizeof rt, f); fclose(f); }
   printf("fb params %u rt %u\n", (unsigned)sizeof fbd, (unsigned)sizeof rt);
}

/* Pack one draw descriptor into dst.  The pointers are parameters so the
 * bytes can be diffed against a capture before we start patching them at
 * submit time. */
static void
pack_draw(void *dst, uint64_t state, uint64_t attrbufs,
          uint64_t attrs, uint64_t varbufs, uint64_t varyings,
          uint64_t viewport, uint64_t tls, uint64_t ubos, uint64_t push,
          uint64_t textures, uint64_t samplers, uint64_t position)
{
   pan_cast_and_pack(dst, DRAW, cfg) {
      cfg.four_components_per_vertex = true;
      cfg.draw_descriptor_is_64b = true;
      cfg.instance_size = 1;
      cfg.instance_primitive_size = 1;
      cfg.state = state;
      cfg.attribute_buffers = attrbufs;
      cfg.attributes = attrs;
      cfg.varying_buffers = varbufs;
      cfg.varyings = varyings;
      cfg.viewport = viewport;
      cfg.thread_storage = tls;
      cfg.uniform_buffers = ubos;
      cfg.push_uniforms = push;
      cfg.textures = textures;
      cfg.samplers = samplers;
      cfg.position = position;
   }
}

/* The blend descriptor that follows the fragment renderer state.  Mesa
 * allocates the fragment state as RENDERER_STATE followed by one BLEND per
 * render target; without it the hardware has no write enable for the tile
 * and the fragment shader's output is dropped.  With blending disabled the
 * equation is a plain replace. */
static void
emit_blend(void)
{
   struct mali_blend_packed blend;
   FILE *f;

   memset(&blend, 0, sizeof blend);
   pan_pack(&blend, BLEND, cfg) {
      cfg.enable = true;
      cfg.round_to_fb_precision = false;
      cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.color_mask = 0xf;
      cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
      cfg.internal.fixed_function.num_comps = 4;
      cfg.internal.fixed_function.rt = 0;
      /* RGBA8 linear, the encoding pan_dithered_format_from_pipe_format
       * returns for PIPE_FORMAT_R8G8B8A8_UNORM. */
      cfg.internal.fixed_function.conversion.memory_format = 0xed000;
      /* The fragment shader writes packed f16, so the blend reads f16. */
      cfg.internal.fixed_function.conversion.register_format =
         MALI_REGISTER_FILE_FORMAT_F16;
   }

   if((f = fopen("blend.bin", "wb")) != NULL){
      fwrite(&blend, 1, sizeof blend, f);
      fclose(f);
   }
   printf("blend %u bytes\n", (unsigned)sizeof blend);
}

/* The job chain: an indexed vertex job carrying both draws, the fragment
 * job that points at the framebuffer, and the tiler context and heap. */
static void
emit_jobs(uint64_t fbd, uint64_t tilerctx, uint64_t heap,
          const void *fragdraw, const void *vertdraw)
{
   struct mali_indexed_vertex_job_packed ivj;
   struct mali_fragment_job_packed fj;
   struct mali_tiler_context_packed tc;
   struct mali_tiler_heap_packed th;
   FILE *f;

   memset(&ivj, 0, sizeof ivj);

   pan_section_pack(&ivj, INDEXED_VERTEX_JOB, HEADER, h) {
      h.is_64b = true;
      h.type = MALI_JOB_TYPE_INDEXED_VERTEX;
      h.index = 1;
   }

   /* 1x1x1 vertices in a 1x3x1 group grid; the shift fields are the bit
    * boundaries of the packed invocation count, so the last one is 32. */
   pan_section_pack(&ivj, INDEXED_VERTEX_JOB, INVOCATION, inv) {
      inv.invocations = 2;
      inv.workgroups_z_shift = 32;
      inv.thread_group_split = 2;
   }

   pan_section_pack(&ivj, INDEXED_VERTEX_JOB, PRIMITIVE, prim) {
      prim.draw_mode = MALI_DRAW_MODE_TRIANGLES;
      prim.first_provoking_vertex = false;
      prim.job_task_split = 6;
      prim.index_count = 3;
   }

   pan_section_pack(&ivj, INDEXED_VERTEX_JOB, PRIMITIVE_SIZE, ps) {
      ps.fixed_sized = 1.0f;
   }

   pan_section_pack(&ivj, INDEXED_VERTEX_JOB, TILER, tp) {
      tp.address = tilerctx;
   }

   /* The two draws live inside the job. */
   memcpy(pan_section_ptr(&ivj, INDEXED_VERTEX_JOB, FRAGMENT_DRAW),
          fragdraw, pan_size(DRAW));
   memcpy(pan_section_ptr(&ivj, INDEXED_VERTEX_JOB, VERTEX_DRAW),
          vertdraw, pan_size(DRAW));

   memset(&fj, 0, sizeof fj);
   pan_section_pack(&fj, FRAGMENT_JOB, HEADER, h) {
      h.is_64b = true;
      h.type = MALI_JOB_TYPE_FRAGMENT;
      h.index = 1;
   }
   pan_section_pack(&fj, FRAGMENT_JOB, PAYLOAD, p) {
      p.bound_max_x = 3;
      p.bound_max_y = 3;
      p.framebuffer = fbd;
   }

   memset(&tc, 0, sizeof tc);
   pan_pack(&tc, TILER_CONTEXT, cfg) {
      cfg.hierarchy_mask = 6;
      cfg.fb_width = 64;
      cfg.fb_height = 64;
      cfg.heap = heap;
   }

   memset(&th, 0, sizeof th);
   pan_pack(&th, TILER_HEAP, cfg) {
      cfg.size = 0x08000000;
      cfg.base = 0x2000000;
      cfg.bottom = 0x2000000;
      cfg.top = 0xa000000;
   }

   if((f = fopen("job_ivj.bin", "wb")) != NULL){ fwrite(&ivj, 1, sizeof ivj, f); fclose(f); }
   if((f = fopen("job_frag.bin", "wb")) != NULL){ fwrite(&fj, 1, sizeof fj, f); fclose(f); }
   if((f = fopen("job_tc.bin", "wb")) != NULL){ fwrite(&tc, 1, sizeof tc, f); fclose(f); }
   if((f = fopen("job_th.bin", "wb")) != NULL){ fwrite(&th, 1, sizeof th, f); fclose(f); }
   printf("jobs ivj %u frag %u tc %u th %u\n", (unsigned)sizeof ivj,
          (unsigned)sizeof fj, (unsigned)sizeof tc, (unsigned)sizeof th);
}

/* One draw descriptor, written to a file for the capture comparison. */
static void
emit_draw(const char *name, uint64_t state, uint64_t attrbufs,
          uint64_t attrs, uint64_t varbufs, uint64_t varyings,
          uint64_t viewport, uint64_t tls, uint64_t ubos, uint64_t push,
          uint64_t textures, uint64_t samplers, uint64_t position)
{
   struct mali_draw_packed draw;
   FILE *f;
   char path[64];

   memset(&draw, 0, sizeof draw);
   pack_draw(&draw, state, attrbufs, attrs, varbufs, varyings, viewport,
             tls, ubos, push, textures, samplers, position);
   snprintf(path, sizeof path, "%s.bin", name);
   if((f = fopen(path, "wb")) != NULL){ fwrite(&draw, 1, sizeof draw, f); fclose(f); }
   printf("%s: draw %u bytes\n", name, (unsigned)sizeof draw);
}

/* The layout around the shaders: attribute buffer and attribute, varying
 * buffer and varying, thread storage and viewport.  Packed with the
 * capture's pointers so the bytes can be diffed against it. */
static void
emit_layout(uint64_t attrptr, uint64_t varyptr)
{
   struct mali_attribute_buffer_packed abuf;
   struct mali_attribute_packed attr;
   struct mali_attribute_buffer_packed vbuf;
   struct mali_attribute_packed vary;
   struct mali_local_storage_packed tls;
   struct mali_viewport_packed vp;
   FILE *f;

   memset(&abuf, 0, sizeof abuf);
   pan_pack(&abuf, ATTRIBUTE_BUFFER, cfg) {
      cfg.type = MALI_ATTRIBUTE_TYPE_1D;
      cfg.pointer = attrptr;
      cfg.stride = 16;
      cfg.size = 48;
   }
   memset(&attr, 0, sizeof attr);
   pan_pack(&attr, ATTRIBUTE, cfg) {
      cfg.format = MALI_PACK_FMT(RGBA32F, RGBA, L);
      cfg.offset = 0;
      cfg.offset_enable = false;
      cfg.buffer_index = 0;
   }
   memset(&vbuf, 0, sizeof vbuf);
   pan_pack(&vbuf, ATTRIBUTE_BUFFER, cfg) {
      cfg.type = MALI_ATTRIBUTE_TYPE_1D;
      cfg.pointer = varyptr;
      cfg.stride = 16;
      cfg.size = 48;
   }
   memset(&vary, 0, sizeof vary);
   pan_pack(&vary, ATTRIBUTE, cfg) {
      cfg.format = MALI_PACK_FMT(RGBA32F, RGBA, L);
      cfg.offset = 0;
      cfg.offset_enable = false;
      cfg.buffer_index = 1;
   }
   memset(&tls, 0, sizeof tls);
   pan_pack(&tls, LOCAL_STORAGE, cfg) {
      cfg.tls_size = 0;
      cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
   }
   memset(&vp, 0, sizeof vp);
   pan_pack(&vp, VIEWPORT, cfg) {
      cfg.minimum_x = -INFINITY;
      cfg.minimum_y = -INFINITY;
      cfg.maximum_x = INFINITY;
      cfg.maximum_y = INFINITY;
      cfg.minimum_z = 0.0f;
      cfg.maximum_z = 1.0f;
      cfg.scissor_maximum_x = 63;
      cfg.scissor_maximum_y = 63;
   }

   if((f = fopen("layout_abuf.bin", "wb")) != NULL){ fwrite(&abuf, 1, sizeof abuf, f); fclose(f); }
   if((f = fopen("layout_attr.bin", "wb")) != NULL){ fwrite(&attr, 1, sizeof attr, f); fclose(f); }
   if((f = fopen("layout_vbuf.bin", "wb")) != NULL){ fwrite(&vbuf, 1, sizeof vbuf, f); fclose(f); }
   if((f = fopen("layout_vary.bin", "wb")) != NULL){ fwrite(&vary, 1, sizeof vary, f); fclose(f); }
   if((f = fopen("layout_tls.bin", "wb")) != NULL){ fwrite(&tls, 1, sizeof tls, f); fclose(f); }
   if((f = fopen("layout_vp.bin", "wb")) != NULL){ fwrite(&vp, 1, sizeof vp, f); fclose(f); }
   printf("layout abuf %u attr %u vbuf %u vary %u tls %u vp %u\n",
          (unsigned)sizeof abuf, (unsigned)sizeof attr,
          (unsigned)sizeof vbuf, (unsigned)sizeof vary,
          (unsigned)sizeof tls, (unsigned)sizeof vp);
}

/* Same callback panvk hands to nir_lower_io. */
static int
cc_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* SPIR-V -> NIR -> the panfrost lowering pipeline, up to the point where
 * pan_shader_compile can take over. */
static nir_shader *
spv_shader(const char *path, mesa_shader_stage stage)
{
   FILE *f = fopen(path, "rb");
   long size;
   uint32_t *words;
   struct spirv_to_nir_options opts = {
      .environment = NIR_SPIRV_OPENGL,
      /* mediump -> fp16, which is what the Bifrost blend wants. */
      .mediump_16bit_alu = true,
   };
   nir_shader *nir;

   if(f == NULL){
      fprintf(stderr, "cannot open %s\n", path);
      exit(1);
   }
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   words = malloc(size);
   if(fread(words, 1, size, f) != (size_t)size){
      fprintf(stderr, "short read on %s\n", path);
      exit(1);
   }
   fclose(f);

   nir = spirv_to_nir(words, size / 4, NULL, 0, stage, "main", &opts,
                      &bifrost_nir_options_v6);
   free(words);
   if(nir == NULL){
      fprintf(stderr, "%s: spirv_to_nir failed\n", path);
      exit(1);
   }

   NIR_PASS(_, nir, nir_lower_system_values);
   /* SPIR-V leaves variable-based I/O; Bifrost wants load/store intrinsics. */
   NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_shader_in | nir_var_shader_out, UINT32_MAX);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            cc_type_size, nir_lower_io_use_interpolated_input_intrinsics);
   if(stage == MESA_SHADER_FRAGMENT)
      NIR_PASS(_, nir, nir_lower_wpos_center);
   pan_optimize_nir(nir, 0x74021000ull);
   pan_preprocess_nir(nir, 0x74021000ull);
   pan_postprocess_nir(nir, 0x74021000ull);
   /* Lowering gl_Position introduces the viewport sysvals, so they are only
    * visible after postprocess. */
   {
      float vp[4] = { 960.0f, 540.0f, 960.0f, 540.0f };
      nir_shader_instructions_pass(nir, lower_viewport_sysvals,
                                   nir_metadata_all, vp);
   }
   nir_shader_instructions_pass(nir, lower_sampler_derefs,
                                nir_metadata_all, NULL);
   /* spirv_to_nir also leaves the sampler/texture counts at zero: those
    * come from the API's binding table, and we have exactly one of each. */
   if(stage == MESA_SHADER_FRAGMENT){
      nir_foreach_function_impl(impl, nir){
         nir_foreach_block(block, impl){
            nir_foreach_instr(instr, block){
               if(instr->type == nir_instr_type_tex){
                  nir->info.num_textures = 1;
                  spv_has_texture = true;
               }
            }
         }
      }
   }

   /* spirv_to_nir does not fill inputs_read, which is what the compiler
    * uses for attribute_count. */
   if(stage == MESA_SHADER_VERTEX)
      nir->info.inputs_read |= 1ull << VERT_ATTRIB_GENERIC0;

   if(getenv("GPUCC_DUMP_NIR"))
      nir_print_shader(nir, stderr);
   return nir;
}

int
main(int argc, char **argv)
{
   glsl_type_singleton_init_or_ref();

   if(argc == 3 && strcmp(argv[1], "dumpfb") == 0){
      dump_fb(argv[2]);
      glsl_type_singleton_decref();
      return 0;
   }
   if(argc == 3){
      nir_shader *fsn = spv_shader(argv[1], MESA_SHADER_FRAGMENT);
      dump("blitfrag", fsn, NULL);

      nir_shader *vsn = spv_shader(argv[2], MESA_SHADER_VERTEX);
      struct pan_varying_layout vl = {0};
      pan_varying_collect_formats(&vl, vsn, 0x74021000ull, true, false);
      pan_build_varying_layout_compact(&vl, vsn, 0x74021000ull);
      dump("blitvert", vsn, &vl);
      glsl_type_singleton_decref();
      return 0;
   }

   /* Fragment: write opaque red to FRAG_RESULT_COLOR. */
   nir_builder fb = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &bifrost_nir_options_v6,
                                                   "gpucc-fs");
   /* The Bifrost blend consumes the colour as two fp16 channels per
    * register (Mesa's capture ends with r0 = 0x00003c00, r1 = 0x3c000000
    * for opaque red).  An fp32 store feeds the blend halves of two
    * registers' worth of garbage instead. */
   nir_store_output(&fb, nir_imm_vec4_16(&fb, 0.25f, 0.5f, 0.75f, 1.0f),
                    nir_imm_int(&fb, 0),
                    .base = 0, .range = 1, .write_mask = 0xf,
                    .src_type = nir_type_float16,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   fb.shader->info.outputs_written |= 1ull << FRAG_RESULT_COLOR;
   nir_validate_shader(fb.shader, "gpucc-fs");
   dump("frag", fb.shader, NULL);

   /* Full-screen triangle (-1,-1) (3,-1) (-1,3), twice: once for the 64x64
    * test target and once for the 1920x1080 scanout.  The viewport
    * transform is baked, so the size is a compile-time constant. */
   emit_vertex("vert", 32.0f, 32.0f);
   emit_vertex("vert1920", 960.0f, 540.0f);
   emit_texture(64, 64);
   emit_layout(0xa008000, 0xa019000);
   emit_framebuffer(0xa004000, 0xa0094c0, 0xa000000);
   emit_blend();
   emit_test_data();
   /* With the captured pointers, so the bytes can be compared. */
   emit_draw("draw_vertex", 0xa002000, 0xa009140, 0xa009160, 0xa0092a0,
             0xa002040, 0, 0xa009100, 0xa0091c0, 0xa0091d0, 0, 0xa009180, 0);
   emit_draw("draw_fragment", 0xa009240, 0, 0, 0, 0,
             0xa009120, 0xa009100, 0xa009220, 0, 0, 0xa009200, 0xa019000);

   {
      struct mali_draw_packed fd, vd;
      memset(&fd, 0, sizeof fd);
      memset(&vd, 0, sizeof vd);
      pack_draw(&fd, 0xa009240, 0, 0, 0, 0, 0xa009120, 0xa009100,
                0xa009220, 0, 0, 0xa009200, 0xa019000);
      pack_draw(&vd, 0xa002000, 0xa009140, 0xa009160, 0xa0092a0,
                0xa002040, 0, 0xa009100, 0xa0091c0, 0xa0091d0, 0,
                0xa009180, 0);
      emit_jobs(0xa009001, 0xa0094c0, 0xa009480, &fd, &vd);
   }

   glsl_type_singleton_decref();
   return 0;
}
