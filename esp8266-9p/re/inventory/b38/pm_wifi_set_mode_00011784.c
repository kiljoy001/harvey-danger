
void pm_wifi_set_mode(void)

{
  undefined4 uVar1;
  
  (*(code *)PTR___wifi_task_suspend_all_00011768)();
  if (((**(char **)PTR_g_wifi_nvs_00011764 == '\x01') && (*(int *)(PTR_g_ic_00011760 + 8) != 0)) &&
     (*(int *)(*(int *)(PTR_g_ic_00011760 + 8) + 0x78) == 5)) {
    (*(code *)PTR___wifi_task_suspend_all_0001176c)();
    uVar1 = 2;
  }
  else if ((((byte)(**(char **)PTR_g_wifi_nvs_00011764 - 2U) < 2) &&
           (*(int *)(PTR_g_ic_00011760 + 0xc) != 0)) &&
          ((*(uint *)(*(int *)(PTR_g_ic_00011760 + 0xc) + 0x104) & 0x100) != 0)) {
    (*(code *)PTR___wifi_task_suspend_all_00011770)();
    uVar1 = 0;
  }
  else {
    (*(code *)PTR___wifi_task_suspend_all_00011774)();
    uVar1 = 1;
  }
  (*(code *)PTR_pm_change_to_state_00011778)(uVar1);
  (*(code *)PTR___wifi_task_resume_all_0001177c)();
  (*(code *)PTR___wifi_task_resume_all_00011780)();
  return;
}

