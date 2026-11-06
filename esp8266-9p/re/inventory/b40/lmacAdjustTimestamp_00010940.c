
void lmacAdjustTimestamp(int param_1)

{
  memw();
  if ((uint)((*(int *)PTR_lmacConfMib_0001093c - *(int *)(PTR_lmacConfMib_0001093c + 4)) * 0x400) <
      (uint)(*DAT_00010938 - *(int *)(*(int *)(param_1 + 0x28) + 0x14))) {
    *(int *)(*(int *)(param_1 + 0x28) + 0x14) =
         *DAT_00010938 +
         (*(int *)PTR_lmacConfMib_0001093c - *(int *)(PTR_lmacConfMib_0001093c + 4)) * -0x400;
  }
  return;
}

