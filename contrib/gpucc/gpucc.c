#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"
#include "panfrost/compiler/bifrost/bifrost_compile.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/lib/pan_shader.h"
#include "panfrost/compiler/pan_compiler.h"
#include "util/u_dynarray.h"
#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
   void *memctx = ralloc_context(NULL);
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &bifrost_nir_options_v6,
                                                  "gpucc");
   nir_store_output(&b, nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f),
                    nir_imm_int(&b, 0),
                    .base = 0, .range = 1, .write_mask = 0xf,
                    .src_type = nir_type_float32,
                    .io_semantics.location = FRAG_RESULT_COLOR,
                    .io_semantics.num_slots = 1);
   b.shader->info.outputs_written |= 1ull << FRAG_RESULT_COLOR;
   nir_validate_shader(b.shader, "gpucc");

   struct pan_compile_inputs inputs = {0};
   inputs.gpu_id = 0x74021000ull;      /* G52, raw GPU_ID */
   inputs.gpu_variant = 0;
   struct util_dynarray bin;
   util_dynarray_init(&bin, memctx);
   struct pan_shader_info info;
   pan_shader_compile(b.shader, &inputs, &bin, &info);

   pan_disassemble(stdout, bin.data, bin.size, inputs.gpu_id, true);
   printf("gpucc: binary %u bytes stage %u tls %llu outputs %#llx\n",
          bin.size, (unsigned)info.stage, (unsigned long long)info.tls_size,
          (unsigned long long)info.outputs_written);
   FILE *f = fopen("frag.bin", "wb");
   if(f != NULL){
      fwrite(bin.data, 1, bin.size, f);
      fclose(f);
   }
   ralloc_free(memctx);
   return 0;
}
