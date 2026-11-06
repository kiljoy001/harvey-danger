
bool lmacMSDUAged(int param_1)

{
  memw();
  return (uint)((*(int *)PTR_lmacConfMib_0001090c + -5) * 0x400) <
         (uint)(*DAT_00010908 - *(int *)(*(int *)(param_1 + 0x28) + 0x14));
}

