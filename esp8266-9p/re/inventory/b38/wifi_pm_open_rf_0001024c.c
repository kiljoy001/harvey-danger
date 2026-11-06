
void wifi_pm_open_rf(void)

{
  undefined8 uVar1;
  
  (*(code *)PTR_esp_sleep_lock_00010234)();
  uVar1 = (*(code *)PTR_esp_timer_get_time_00010238)();
  *(undefined8 *)PTR_DAT_0001022c = uVar1;
  (*(code *)PTR_clockgate_watchdog_0001023c)(1);
  (*(code *)PTR_phy_open_rf_00010240)();
  (*(code *)PTR_clockgate_watchdog_00010244)(0);
  if (*PTR_pend_flag_noise_check_00010230 == '\x01') {
    *PTR_pend_flag_noise_check_00010230 = 0;
    (*(code *)PTR_noise_check_loop_00010248)(1,1);
  }
  return;
}

