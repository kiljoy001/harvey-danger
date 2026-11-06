
void lmacInitAc(byte param_1,undefined1 param_2,undefined1 param_3,undefined1 param_4,
               undefined2 param_5)

{
  undefined *puVar1;
  
  puVar1 = PTR_DAT_0001063c;
  PTR_DAT_0001063c[(uint)param_1 * 0x24 + 4] = param_1;
  puVar1[(uint)param_1 * 0x24 + 9] = 0;
  puVar1[(uint)param_1 * 0x24 + 10] = 0;
  puVar1[(uint)param_1 * 0x24 + 0x11] = 0;
  puVar1[(uint)param_1 * 0x24 + 5] = param_2;
  puVar1[(uint)param_1 * 0x24 + 7] = param_3;
  puVar1[(uint)param_1 * 0x24 + 8] = param_4;
  puVar1[(uint)param_1 * 0x24 + 6] = param_3;
  *(undefined2 *)(puVar1 + (uint)param_1 * 0x24 + 0x18) = param_5;
  *(undefined4 *)(puVar1 + (uint)param_1 * 0x24 + 0x1c) = 0;
  *(undefined4 *)(puVar1 + (uint)param_1 * 0x24 + 0x20) = 0;
  return;
}

