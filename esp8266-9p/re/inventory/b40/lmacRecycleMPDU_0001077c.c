
void lmacRecycleMPDU(undefined4 param_1,int param_2,undefined1 param_3)

{
  *(uint *)(*(int *)(param_2 + 0x28) + 4) =
       *(uint *)(*(int *)(param_2 + 0x28) + 4) & DAT_00010770 | DAT_00010774;
  (*(code *)PTR_FUN_00010778)(param_2,param_3);
  return;
}

