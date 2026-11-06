
void lmacSetAcParam(uint param_1,undefined1 param_2,uint param_3,uint param_4,undefined2 param_5)

{
  undefined *puVar1;
  
  param_1 = param_1 & 0xff;
  param_3 = param_3 & 0xff;
  param_4 = param_4 & 0xff;
  (*(code *)PTR_vPortETSIntrLock_000105c8)();
  puVar1 = PTR_DAT_000105c4;
  PTR_DAT_000105c4[param_1 * 0x24 + 5] = param_2;
  puVar1[param_1 * 0x24 + 7] = (char)param_3;
  puVar1[param_1 * 0x24 + 8] = (char)param_4;
  if (param_4 < (byte)puVar1[param_1 * 0x24 + 6]) {
    puVar1[param_1 * 0x24 + 6] = (char)param_4;
  }
  else if ((byte)puVar1[param_1 * 0x24 + 6] < param_3) {
    puVar1[param_1 * 0x24 + 6] = (char)param_3;
  }
  *(undefined2 *)(puVar1 + param_1 * 0x24 + 0x18) = param_5;
  (*(code *)PTR_vPortETSIntrUnlock_000105cc)();
  return;
}

