#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/lib/pan_shader.h"
#include "panfrost/compiler/bifrost/bifrost/disassemble.h"
#include "spirv/nir_spirv.h"
#include "glsl_types.h"

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
      cfg.format = MALI_RGBA8_UNORM;
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

   glsl_type_singleton_decref();
   return 0;
}
