
void esp_wifi_ps_unlock(void)

{
  *(uint *)PTR_app_optimize_and_debug_mac_000101e8 =
       *(uint *)PTR_app_optimize_and_debug_mac_000101e8 & 0xfffffffe;
  return;
}

