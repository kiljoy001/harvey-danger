
void ic_set_vif(uint param_1,char param_2,undefined4 param_3,undefined4 param_4)

{
  int iVar1;
  
  param_1 = param_1 & 0xff;
  if (param_2 == '\x02') {
    *(undefined4 *)(PTR_if_ctrl_000101d0 + param_1 * 0x10 + 8) = param_4;
  }
  else if (param_2 == '\x01') {
    (*(code *)PTR_wDev_SetMacAddress_000101e0)(param_1,param_3);
    iVar1 = (*(code *)PTR_ic_enable_interface_000101e4)(param_1,param_3,param_4);
    if (iVar1 == 1) {
      memw();
      memw();
      *DAT_000101d4 = *DAT_000101d4 | DAT_000101d8;
    }
    (*(code *)PTR_FUN_000101e8)(param_1);
  }
  else {
    (*(code *)PTR_wDev_SetRxPolicy_000101ec)(0,param_1);
    iVar1 = (*(code *)PTR_ic_disable_interface_000101f0)(param_1);
    if (iVar1 == 0) {
      memw();
      memw();
      *DAT_000101d4 = *DAT_000101d4 & DAT_000101dc;
    }
  }
  return;
}

