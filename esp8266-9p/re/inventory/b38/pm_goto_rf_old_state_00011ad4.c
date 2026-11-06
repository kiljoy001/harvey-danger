
void pm_goto_rf_old_state(void)

{
  int iVar1;
  undefined4 uVar2;
  undefined *puVar3;
  
  (*(code *)PTR___wifi_task_suspend_all_00011abc)();
  iVar1 = *(int *)(PTR_DAT_00011ab0 + 0x70);
  puVar3 = PTR_DAT_00011ab0;
  if (iVar1 == 1) {
LAB_00011aec:
    uVar2 = 0;
  }
  else {
    if (iVar1 != 6) {
      if (iVar1 != 0) {
        *(int *)(PTR_DAT_00011ab0 + 0x6c) = iVar1;
        goto LAB_00011b4f;
      }
      if (((**(char **)PTR_g_wifi_nvs_00011ab8 == '\x01') && (*(int *)(PTR_g_ic_00011ab4 + 8) != 0))
         && (*(int *)(*(int *)(PTR_g_ic_00011ab4 + 8) + 0x78) == 5)) {
        (*(code *)PTR_pm_send_nullfunc_00011ac0)();
      }
      goto LAB_00011aec;
    }
    uVar2 = 4;
  }
  *(undefined4 *)(puVar3 + 0x6c) = uVar2;
  (*(code *)PTR___wifi_task_suspend_all_00011ac4)();
  (*(code *)PTR_pm_change_to_state_00011ac8)(*(undefined4 *)(puVar3 + 0x70));
  (*(code *)PTR___wifi_task_resume_all_00011acc)();
LAB_00011b4f:
  (*(code *)PTR___wifi_task_resume_all_00011ad0)();
  return;
}

