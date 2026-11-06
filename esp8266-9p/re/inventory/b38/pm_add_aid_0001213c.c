
bool pm_add_aid(int param_1)

{
  bool bVar1;
  
  bVar1 = *(short *)(param_1 + 2) == 0;
  if (bVar1) {
    *(undefined2 *)(PTR_DAT_00012138 + 0x80) = *(undefined2 *)(param_1 + 4);
  }
  return bVar1;
}

