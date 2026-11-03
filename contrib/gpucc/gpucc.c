#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/lib/pan_shader.h"
#include "util/u_dynarray.h"
#include <stdio.h>
#include <stdlib.h>

static void
dump(const char *name, nir_shader *nir, struct pan_varying_layout *layout)
{
   struct pan_compile_inputs inputs = {0};
   inputs.gpu_id = 0x74021000ull;      /* G52, raw GPU_ID */
   inputs.gpu_variant = 0;
   inputs.varying_layout = layout;
   inputs.no_idvs = (nir->info.stage == MESA_SHADER_VERTEX);

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
}

int
main(void)
{
   /* Fragment: write opaque red to FRAG_RESULT_COLOR. */
   nir_builder fb = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   &bifrost_nir_options_v6,
                                                   "gpucc-fs");
   nir_store_output(&fb, nir_imm_vec4(&fb, 1.0f, 0.0f, 0.0f, 1.0f),
                    nir_imm_int(&fb, 0),
                    .base = 0, .range = 1, .write_mask = 0xf,
                    .src_type = nir_type_float32,
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
   nir_store_output(&vb, pos, nir_imm_int(&vb, 0),
                    .base = 0, .range = 1, .write_mask = 0xf,
                    .src_type = nir_type_float32,
                    .io_semantics.location = VARYING_SLOT_POS,
                    .io_semantics.num_slots = 1);
   vb.shader->info.outputs_written |= 1ull << VARYING_SLOT_POS;
   nir_validate_shader(vb.shader, "gpucc-vs");
   struct pan_varying_layout layout = {0};
   pan_varying_collect_formats(&layout, vb.shader, 0x74021000ull, true, false);
   pan_build_varying_layout_compact(&layout, vb.shader, 0x74021000ull);
   dump("vert", vb.shader, &layout);

   return 0;
}
