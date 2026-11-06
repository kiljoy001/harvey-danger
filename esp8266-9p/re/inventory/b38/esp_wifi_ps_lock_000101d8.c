
void esp_wifi_ps_lock(void)

{
  *(uint *)PTR_app_optimize_and_debug_mac_000101d4 =
       *(uint *)PTR_app_optimize_and_debug_mac_000101d4 | 1;
  return;
}

