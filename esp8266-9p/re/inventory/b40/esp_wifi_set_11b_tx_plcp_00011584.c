
void esp_wifi_set_11b_tx_plcp(undefined1 param_1,undefined1 param_2)

{
  *PTR_DAT_0001157c = param_2;
  *PTR_DAT_00011580 = param_1;
  return;
}

