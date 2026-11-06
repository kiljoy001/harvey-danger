
void FUN_40220b0c(int param_1,int param_2)

{
  undefined *puVar1;
  int *piVar2;
  
  if ((param_2 == 1) || (param_2 == 2)) {
    (*DAT_402010d0)(*(undefined4 *)(param_1 + 0x24),0,0x20);
    (*DAT_40201d38)();
    piVar2 = DAT_4022091c;
    *(int *)(param_1 + 0x20) = *DAT_4022091c;
    *piVar2 = param_1;
    (*DAT_40201d40)();
  }
  else if (param_2 == 4) {
    puVar1 = PTR_s_esf_buf_c_40220920;
    (*(code *)PTR_mem_free_fileline_40201778)
              (*(undefined4 *)(param_1 + 4),PTR_s_esf_buf_c_40220920,0x1b7);
    (*(code *)PTR_mem_free_fileline_40201778)(*(undefined4 *)(param_1 + 0x24),puVar1,0x1b9);
    (*(code *)PTR_mem_free_fileline_40201778)(*(undefined4 *)(param_1 + 0x10),puVar1,0x1ba);
    (*(code *)PTR_mem_free_fileline_40201778)(param_1,puVar1,0x1bb);
  }
  else if (param_2 == 5) {
    (*DAT_402010d0)(*(undefined4 *)(param_1 + 0x24),0,0x20);
    (*DAT_40201d38)();
    piVar2 = DAT_4022091c;
    *(int *)(param_1 + 0x20) = DAT_4022091c[2];
    piVar2[2] = param_1;
    (*DAT_40201d40)();
  }
  else if (param_2 == 6) {
    puVar1 = PTR_s_esf_buf_c_40220920;
    (*(code *)PTR_mem_free_fileline_40201778)
              (*(undefined4 *)(param_1 + 4),PTR_s_esf_buf_c_40220920,0x1cc);
    (*(code *)PTR_mem_free_fileline_40201778)(*(undefined4 *)(param_1 + 0x24),puVar1,0x1ce);
    (*(code *)PTR_mem_free_fileline_40201778)(*(undefined4 *)(param_1 + 0x10),puVar1,0x1cf);
    (*(code *)PTR_mem_free_fileline_40201778)(param_1,puVar1,0x1d0);
  }
  else if (param_2 == 7) {
    (*DAT_402010d0)(*(undefined4 *)(param_1 + 0x24),0,0x20);
    piVar2 = DAT_4022091c;
    (*DAT_40201d38)();
    *(int *)(param_1 + 0x20) = piVar2[3];
    piVar2[3] = param_1;
    (*DAT_40201d40)();
  }
  else if (param_2 == 8) {
    (*DAT_402010d0)(*(undefined4 *)(param_1 + 0x24),0,0xc);
    (*DAT_40201d38)();
    piVar2 = DAT_4022091c;
    *(int *)(param_1 + 0x20) = DAT_4022091c[4];
    piVar2[4] = param_1;
    (*DAT_40201d40)();
  }
  return;
}

