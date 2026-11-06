
void pm_goto_rf_off(void)

{
  (*(code *)PTR___wifi_task_suspend_all_00011638)();
  (*(code *)PTR_os_timer_disarm_0001163c)(PTR_DAT_00011630);
  (*(code *)PTR_os_timer_disarm_00011640)(PTR_DAT_00011634);
  (*(code *)PTR___wifi_task_suspend_all_00011644)();
  (*(code *)PTR_pm_change_to_state_00011648)(1);
  (*(code *)PTR___wifi_task_resume_all_0001164c)();
  (*(code *)PTR___wifi_task_resume_all_00011650)();
  return;
}

