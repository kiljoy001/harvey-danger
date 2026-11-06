
void pm_change_to_state(uint param_1)

{
  byte bVar1;
  int iVar2;
  undefined4 uVar3;
  uint uVar4;
  int iVar5;
  undefined *puVar6;
  undefined *puVar7;
  uint uVar8;
  
  bVar1 = **(byte **)PTR_g_wifi_nvs_00010fbc;
  if ((((byte)(bVar1 - 2) < 2) && (*(int *)(PTR_g_ic_00010fc0 + 0xc) != 0)) &&
     ((*(uint *)(*(int *)(PTR_g_ic_00010fc0 + 0xc) + 0x104) & 0x100) != 0)) {
    *(uint *)(PTR_DAT_00010fc4 + 0x70) = param_1;
    param_1 = 0;
  }
  puVar6 = PTR_DAT_00010fc4;
  puVar7 = PTR_g_ic_00010fc0;
  if ((**(char **)PTR_sniffer_enable_00010fc8 != '\0') && (param_1 != 7)) {
    return;
  }
  if (PTR_g_ic_00010fc0[0x160] != '\0') {
    if (param_1 == 7) {
      return;
    }
LAB_000110a4:
    *(uint *)(PTR_DAT_00010fc4 + 0x70) = param_1;
    return;
  }
  uVar8 = *(uint *)(PTR_DAT_00010fc4 + 0x6c);
  if (uVar8 == 7) goto LAB_000110a4;
  if ((param_1 == uVar8) && (param_1 != 6)) {
    return;
  }
  if (param_1 == 2) {
    if (uVar8 == 0) goto LAB_000111ce;
    if (uVar8 == 5) {
      *(undefined4 *)(PTR_DAT_00010fc4 + 0x6c) = 2;
      (*(code *)PTR_ppProcessWaitQ_00011000)();
      return;
    }
    if (uVar8 != 4) {
      if (uVar8 == 6) {
        (*(code *)PTR_os_timer_disarm_00011004)(PTR_DAT_00010fcc);
        (*(code *)PTR_wifi_pm_open_rf_00011008)();
      }
      else if (uVar8 != 3) {
        return;
      }
    }
    (*(code *)PTR_pm_send_nullfunc_0001100c)(0);
    uVar3 = 5;
    goto LAB_000113a2;
  }
  if (param_1 < 3) {
    if (param_1 == 0) {
      if (uVar8 == 6) {
        (*(code *)PTR_os_timer_disarm_00010fe4)(PTR_DAT_00010fcc);
        (*(code *)PTR_wifi_pm_open_rf_00010fe8)();
        (*(code *)PTR_pm_send_nullfunc_00010fec)(param_1);
      }
      else if (uVar8 == 1) {
        (*(code *)PTR_wifi_pm_open_rf_00010ff0)();
      }
      uVar3 = 0;
    }
    else {
      if (param_1 != 1) {
        return;
      }
      if (uVar8 == 6) goto LAB_000111ce;
      if ((((bVar1 & 0xfd) == 1) && (*(int *)(PTR_g_ic_00010fc0 + 8) != 0)) &&
         (*(int *)(*(int *)(PTR_g_ic_00010fc0 + 8) + 0x78) == 5)) {
        return;
      }
      iVar2 = (*(code *)PTR_ppCheckTxIdle_00010ff4)(1);
      if ((((iVar2 != 0) || ((byte)puVar6[0x85] < (byte)puVar6[0x89])) ||
          ((byte)puVar6[0x84] < (byte)puVar6[0x88])) || ((byte)puVar6[0x83] < (byte)puVar6[0x87])) {
LAB_00011165:
        (*(code *)PTR_pm_wait_tx_rx_idle_fun_00010ffc)();
        return;
      }
      (*(code *)PTR_wifi_pm_close_rf_00010ff8)();
      uVar3 = 1;
    }
    goto LAB_000113a2;
  }
  if (param_1 == 6) {
    if (uVar8 - 3 < 2) {
      iVar2 = (*(code *)PTR_ppCheckTxIdle_00011020)(1);
      if (((iVar2 != 0) || ((byte)puVar6[0x85] < (byte)puVar6[0x89])) ||
         (((byte)puVar6[0x84] < (byte)puVar6[0x88] || ((byte)puVar6[0x83] < (byte)puVar6[0x87]))))
      goto LAB_00011165;
      memw();
      iVar5 = *(int *)PTR_DAT_00010fd8;
      iVar2 = *(int *)PTR_WdevTimOffSet_00010fd4;
      uVar8 = *(int *)(puVar6 + 0x5c) + 32000;
      if (uVar8 <= (uint)(*DAT_00010fd0 + iVar5 + iVar2)) {
        return;
      }
      memw();
      if (DAT_00010fdc < ((uVar8 - *DAT_00010fd0) - iVar5) - iVar2) {
        return;
      }
      memw();
      if (((uVar8 - *DAT_00010fd0) - iVar5) - iVar2 <= DAT_00010fe0) {
        return;
      }
      memw();
      (*(code *)PTR_os_timer_arm_us_00011024)
                (PTR_DAT_00010fcc,((uVar8 - *DAT_00010fd0) - iVar5) - iVar2,0);
      (*(code *)PTR_wifi_pm_close_rf_00011028)();
      uVar3 = 6;
      goto LAB_000113a2;
    }
    if (uVar8 != 2) {
      if (uVar8 != 6) {
        return;
      }
      memw();
      iVar5 = *(int *)PTR_DAT_00010fd8;
      iVar2 = *(int *)PTR_WdevTimOffSet_00010fd4;
      uVar4 = *(int *)(PTR_DAT_00010fc4 + 0x5c) + 32000;
      if (uVar4 <= (uint)(*DAT_00010fd0 + iVar5 + iVar2)) {
        return;
      }
      memw();
      if (DAT_00010fdc < ((uVar4 - *DAT_00010fd0) - iVar5) - iVar2) {
        return;
      }
      memw();
      if (((uVar4 - *DAT_00010fd0) - iVar5) - iVar2 <= DAT_00010fe0) {
        return;
      }
      memw();
      puVar7 = PTR_DAT_00010fc4;
      (*(code *)PTR_os_timer_arm_us_00011030)
                (PTR_DAT_00010fcc,((uVar4 - *DAT_00010fd0) - iVar5) - iVar2,0);
      *(uint *)(puVar7 + 0x6c) = uVar8;
      return;
    }
    iVar2 = (*(code *)PTR_ppCheckTxIdle_0001102c)(1);
    if (iVar2 != 0) {
      return;
    }
    if ((byte)puVar6[0x85] < (byte)puVar6[0x89]) {
      return;
    }
    if ((byte)puVar6[0x84] < (byte)puVar6[0x88]) {
      return;
    }
    if ((byte)puVar6[0x83] < (byte)puVar6[0x87]) {
      return;
    }
  }
  else {
    if (param_1 == 7) {
      *(uint *)(PTR_DAT_00010fc4 + 0x70) = uVar8;
      if (uVar8 != 4) {
        if ((uVar8 == 6) || (uVar8 == 1)) {
          (*(code *)PTR_os_timer_disarm_00011034)(PTR_DAT_00010fcc);
          (*(code *)PTR_wifi_pm_open_rf_00011038)();
        }
        else {
          if ((uVar8 != 2) && (uVar8 != 5)) {
            if (uVar8 != 0) {
              return;
            }
            if (((*(int *)(puVar7 + 8) == 0) || (bVar1 != 1)) ||
               (*(int *)(*(int *)(puVar7 + 8) + 0x78) != 5)) goto LAB_000113a0;
          }
          (*(code *)PTR_pm_send_nullfunc_0001103c)(1);
        }
      }
LAB_000113a0:
      uVar3 = 7;
      goto LAB_000113a2;
    }
    if (param_1 != 4) {
      return;
    }
    if (uVar8 == 3) {
LAB_000111ce:
      *(uint *)(puVar6 + 0x6c) = param_1;
      return;
    }
    if (uVar8 == 6) {
      (*(code *)PTR_os_timer_disarm_00011010)(PTR_DAT_00010fcc);
      (*(code *)PTR_wifi_pm_open_rf_00011014)();
      goto LAB_000111ce;
    }
    if (uVar8 != 2) {
      return;
    }
    iVar2 = (*(code *)PTR_ppCheckTxIdle_00011018)(1);
    if (iVar2 != 0) {
      return;
    }
  }
  (*(code *)PTR_pm_send_nullfunc_0001101c)(1);
  uVar3 = 3;
LAB_000113a2:
  *(undefined4 *)(puVar6 + 0x6c) = uVar3;
  return;
}

