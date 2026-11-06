
void esp_wifi_internal_set_tx_param(int param_1,int param_2,int param_3,char param_4,char param_5)

{
  if (param_2 != 0) {
    *(int *)PTR_lmacConfMib_00010968 = param_2;
  }
  if (param_1 != 0) {
    *(int *)(PTR_lmacConfMib_00010968 + 4) = param_1;
  }
  if (param_3 != 0) {
    *(int *)(PTR_lmacConfMib_00010968 + 8) = param_3;
  }
  if (param_4 != '\0') {
    PTR_lmacConfMib_00010968[0xd] = param_4;
  }
  if (param_5 != '\0') {
    PTR_lmacConfMib_00010968[0xc] = param_4;
  }
  return;
}

