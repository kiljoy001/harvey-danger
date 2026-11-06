
void pm_open_rf_for_bcn_fun(void)

{
  undefined *puVar1;
  
  (*(code *)PTR___wifi_task_suspend_all_00011a44)();
  puVar1 = PTR_DAT_00011a3c;
  (*(code *)PTR_os_timer_disarm_00011a48)(PTR_DAT_00011a3c);
  if (*(int *)(puVar1 + 0x6c) == 6) {
    (*(code *)PTR___wifi_task_suspend_all_00011a4c)();
    (*(code *)PTR_pm_change_to_state_00011a50)(4);
    (*(code *)PTR___wifi_task_resume_all_00011a54)();
    (*(code *)PTR_os_timer_arm_us_00011a58)(puVar1,*(undefined4 *)PTR_DAT_00011a40,0);
  }
  (*(code *)PTR___wifi_task_resume_all_00011a5c)();
  return;
}

