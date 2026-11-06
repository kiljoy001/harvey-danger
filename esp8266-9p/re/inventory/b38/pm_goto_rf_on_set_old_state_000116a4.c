
void pm_goto_rf_on_set_old_state(void)

{
  if (*(int *)(PTR_DAT_00011694 + 0x6c) != 7) {
    *(int *)(PTR_DAT_00011694 + 0x70) = *(int *)(PTR_DAT_00011694 + 0x6c);
  }
  (*(code *)PTR___wifi_task_suspend_all_00011698)();
  (*(code *)PTR_pm_change_to_state_0001169c)(0);
  (*(code *)PTR___wifi_task_resume_all_000116a0)();
  return;
}

