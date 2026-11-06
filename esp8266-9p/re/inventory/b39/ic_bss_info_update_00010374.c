
void ic_bss_info_update(undefined1 param_1,undefined4 param_2,short param_3,int param_4)

{
  if (param_3 == 2) {
    if (param_4 == 0) {
LAB_00010394:
      (*(code *)PTR_FUN_00010370)();
      return;
    }
  }
  else if (param_3 != 1) goto LAB_00010394;
  (*(code *)PTR_wDev_SetRxPolicy_0001036c)(2,param_1,param_2);
  return;
}

