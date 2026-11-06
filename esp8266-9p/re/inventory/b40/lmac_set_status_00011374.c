
void lmac_set_status(int param_1,int param_2)

{
  undefined *puVar1;
  
  puVar1 = PTR_DAT_00011370;
  if (param_2 == 0) {
    *(int *)(PTR_DAT_00011370 + param_1 * 0x24 + 0x1c) =
         *(int *)(PTR_DAT_00011370 + param_1 * 0x24 + 0x1c) + 1;
  }
  else {
    *(int *)(PTR_DAT_00011370 + param_1 * 0x24 + 0x20) =
         *(int *)(PTR_DAT_00011370 + param_1 * 0x24 + 0x20) + 1;
  }
  puVar1[param_1 * 0x24 + 0x11] = 2;
  return;
}

