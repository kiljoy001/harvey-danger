
void pm_scan_stop(void)

{
  (*(code *)PTR___wifi_task_suspend_all_00011b64)();
  if (*(int *)(PTR_DAT_00011b60 + 0x6c) == 7) {
    (*(code *)PTR_pm_goto_rf_old_state_00011b68)();
  }
  (*(code *)PTR___wifi_task_resume_all_00011b6c)();
  return;
}

