
void esp_wifi_get_pm_config(undefined1 *param_1)

{
  undefined *puVar1;
  
  puVar1 = PTR_DAT_000100ac;
  if (param_1 != (undefined1 *)0x0) {
    *param_1 = PTR_DAT_000100ac[0x8a];
    param_1[1] = puVar1[0x8b];
    param_1[2] = puVar1[0x86];
    param_1[3] = puVar1[0x87];
    param_1[4] = puVar1[0x88];
    param_1[5] = puVar1[0x89];
    param_1[6] = puVar1[0x8c];
    param_1[7] = puVar1[0x8d];
  }
  return;
}

