
void ic_set_sta(uint param_1,char param_2,undefined4 param_3,undefined1 param_4,undefined2 param_5,
               undefined4 param_6)

{
  int iVar1;
  int iVar2;
  undefined4 uVar3;
  
  param_1 = param_1 & 0xff;
  if (param_2 == '\0') {
    (*(code *)PTR_rc_disable_trc_000102a4)(param_1,param_3,param_4,param_5);
    if (param_1 != 1) {
      return;
    }
    (*(code *)PTR_vPortETSIntrLock_000102b0)();
    if (PTR_g_ic_0001028c[0x174] != '\0') {
      PTR_g_ic_0001028c[0x174] = PTR_g_ic_0001028c[0x174] + -1;
    }
  }
  else {
    iVar1 = (*(code *)PTR_rc_enable_trc_00010290)(param_1,param_3);
    if (iVar1 != 0) {
      *(undefined4 *)(iVar1 + 0x30) = param_6;
      (*(code *)PTR_vPortETSIntrLock_00010294)();
      iVar2 = (*(code *)PTR_esp_wifi_try_rate_from_high_00010298)();
      uVar3 = 0x21;
      if (iVar2 == 0) {
        uVar3 = 0;
      }
      (*(code *)PTR_rcUpdatePhyMode_0001029c)(iVar1,uVar3);
      (*(code *)PTR_vPortETSIntrUnlock_000102a0)();
    }
    if (param_1 != 1) {
      return;
    }
    (*(code *)PTR_vPortETSIntrLock_000102ac)();
    PTR_g_ic_0001028c[0x174] = PTR_g_ic_0001028c[0x174] + '\x01';
  }
  (*(code *)PTR_vPortETSIntrUnlock_000102a8)();
  return;
}

