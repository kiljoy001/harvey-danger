
void FUN_00010014(int param_1,int param_2)

{
  (*(code *)PTR_ppEnqueueTxDone_00010004)();
  if ((**(uint **)(param_1 + 0x28) & 8) != 0) {
    (*(code *)PTR_rcUpdateTxDone_00010008)(*(undefined4 *)(param_1 + 0x20));
  }
  (*(code *)PTR_pp_post_0001000c)(4,0);
  if (param_2 != 0) {
    if (PTR_DAT_00010000[(*(uint *)(*(int *)(param_1 + 0x28) + 8) >> 0x14 & 7) * 0x24 + 0x11] ==
        '\0') {
      (*(code *)PTR_ppProcessTxQ_00010010)();
    }
  }
  return;
}

