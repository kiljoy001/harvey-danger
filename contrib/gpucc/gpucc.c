#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/lib/pan_shader.h"
#include "panfrost/compiler/bifrost/bifrost/disassemble.h"
#include "util/u_dynarray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
          "work_regs %u uniform_preload %u\n",
          name, (unsigned)sizeof rsd,
          (unsigned long long)shader_placeholder,
          info->attribute_count, info->varyings.formats.count,
          info->ubo_count, info->work_reg_count,
          info->push.count);
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

int
main(void)
{
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

   /* Vertex: the full-screen triangle (-1,-1) (3,-1) (-1,3). */
   nir_builder vb = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
                                                   &bifrost_nir_options_v6,
                                                   "gpucc-vs");
   nir_def *pos = nir_load_input(&vb, 4, 32, nir_imm_int(&vb, 0),
                                 .base = 0,
                                 .io_semantics.location = VERT_ATTRIB_GENERIC0,
                                 .io_semantics.num_slots = 1);
   /* The tiler path wants the position already in framebuffer pixels;
    * Mesa does this with uniforms, we bake it for the target size. */
   nir_def *sx = nir_imm_float(&vb, 32.0f);
   nir_def *half = nir_imm_float(&vb, 0.5f);
   nir_def *px = nir_fadd(&vb, nir_fmul(&vb, nir_channel(&vb, pos, 0), sx), sx);
   nir_def *py = nir_fadd(&vb, nir_fmul(&vb, nir_channel(&vb, pos, 1), sx), sx);
   nir_def *pz = nir_fadd(&vb, nir_fmul(&vb, nir_channel(&vb, pos, 2), half), half);
   nir_def *out = nir_vec4(&vb, px, py, pz, nir_channel(&vb, pos, 3));
   nir_store_output(&vb, out, nir_imm_int(&vb, 0),
                    .base = 0, .range = 1, .write_mask = 0xf,
                    .src_type = nir_type_float32,
                    .io_semantics.location = VARYING_SLOT_POS,
                    .io_semantics.num_slots = 1);
   vb.shader->info.outputs_written |= 1ull << VARYING_SLOT_POS;
   /* The compiler derives attribute_count from inputs_read, which
    * nir_load_input does not set on its own. */
   vb.shader->info.inputs_read |= 1ull << VERT_ATTRIB_GENERIC0;
   nir_validate_shader(vb.shader, "gpucc-vs");
   struct pan_varying_layout layout = {0};
   pan_varying_collect_formats(&layout, vb.shader, 0x74021000ull, true, false);
   pan_build_varying_layout_compact(&layout, vb.shader, 0x74021000ull);
   dump("vert", vb.shader, &layout);

   return 0;
}
