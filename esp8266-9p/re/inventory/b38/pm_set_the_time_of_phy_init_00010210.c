
void pm_set_the_time_of_phy_init(void)

{
  undefined8 uVar1;
  
  uVar1 = (*(code *)PTR_esp_timer_get_time_0001020c)();
  *(undefined8 *)PTR_DAT_00010208 = uVar1;
  return;
}

