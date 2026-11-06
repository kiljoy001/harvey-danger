
void pm_goto_rf_on(void)

{
  (*(code *)PTR___wifi_task_suspend_all_000116d4)();
  (*(code *)PTR_pm_change_to_state_000116d8)(0);
  (*(code *)PTR___wifi_task_resume_all_000116dc)();
  return;
}

