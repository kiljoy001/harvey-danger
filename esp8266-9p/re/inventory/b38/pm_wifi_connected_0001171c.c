
void pm_wifi_connected(void)

{
  byte bVar1;
  
  bVar1 = (*(code *)PTR_wifi_nvs_get_sta_listen_interval_00011708)();
  if (bVar1 < 0xb) {
    PTR_DAT_00011704[0x78] = bVar1;
  }
  else {
    PTR_DAT_00011704[0x78] = 10;
  }
  (*(code *)PTR_pm_reset_param_0001170c)();
  (*(code *)PTR___wifi_task_suspend_all_00011710)();
  (*(code *)PTR_pm_change_to_state_00011714)(2);
  (*(code *)PTR___wifi_task_resume_all_00011718)();
  return;
}

