
void pm_wait_tx_rx_idle_fun(void)

{
  char cVar1;
  char cVar2;
  byte bVar3;
  byte bVar4;
  byte bVar5;
  int iVar6;
  undefined4 uVar7;
  undefined *puVar8;
  undefined1 uVar9;
  uint uVar10;
  
  (*(code *)PTR___wifi_task_suspend_all_00011810)();
  uVar10 = 0;
  (*(code *)PTR_os_timer_disarm_00011814)(PTR_DAT_000117f8);
  puVar8 = PTR_DAT_00011800;
  *PTR_DAT_000117fc = 0;
  if (*(int *)(puVar8 + 0x6c) == 6) {
    uVar10 = 1;
  }
  if (((uVar10 & 0xff) != 0) || (uVar9 = *(int *)(puVar8 + 0x6c) == 1, (bool)uVar9))
  goto LAB_00011878;
  iVar6 = (*(code *)PTR_ppCheckTxIdle_0001181c)();
  if (iVar6 == 0) {
    puVar8[0x83] = puVar8[0x83] + '\x01';
  }
  else {
    puVar8[0x83] = uVar9;
    (*(code *)PTR_ppProcessWaitQ_00011820)();
  }
  cVar1 = puVar8[0x84];
  cVar2 = puVar8[0x85];
  bVar3 = puVar8[0x88];
  puVar8[0x84] = cVar1 + 1U;
  puVar8[0x85] = cVar2 + 1U;
  if (bVar3 < (byte)(cVar1 + 1U)) {
    puVar8[0x84] = bVar3;
  }
  bVar4 = puVar8[0x89];
  if (bVar4 < (byte)(cVar2 + 1U)) {
    puVar8[0x85] = bVar4;
  }
  bVar5 = puVar8[0x87];
  if (bVar5 < (byte)puVar8[0x83]) {
    puVar8[0x83] = bVar5;
  }
  if ((((byte)puVar8[0x84] < bVar3) || ((byte)puVar8[0x85] < bVar4)) || ((byte)puVar8[0x83] < bVar5)
     ) {
LAB_00011910:
    (*(code *)PTR_pm_set_idle_timer_00011828)();
  }
  else {
    if (((**(char **)PTR_g_wifi_nvs_00011808 == '\x01') && (*(int *)(PTR_g_ic_00011804 + 8) != 0))
       && (*(int *)(*(int *)(PTR_g_ic_00011804 + 8) + 0x78) == 5)) {
      if ((*(uint *)PTR_app_optimize_and_debug_mac_0001180c & 1) != 0) goto LAB_00011910;
      (*(code *)PTR___wifi_task_suspend_all_00011824)();
      uVar7 = 6;
    }
    else {
      if ((*(uint *)PTR_app_optimize_and_debug_mac_0001180c & 1) != 0) goto LAB_00011910;
      (*(code *)PTR___wifi_task_suspend_all_0001182c)();
      uVar7 = 1;
    }
    (*(code *)PTR_pm_change_to_state_00011830)(uVar7);
    (*(code *)PTR___wifi_task_resume_all_00011834)();
  }
LAB_00011878:
  (*(code *)PTR___wifi_task_resume_all_00011818)();
  return;
}

