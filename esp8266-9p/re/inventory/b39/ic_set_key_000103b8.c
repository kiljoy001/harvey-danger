
void ic_set_key(uint param_1,byte param_2,undefined1 param_3,undefined4 param_4,byte param_5,
               undefined4 param_6,undefined4 param_7,byte param_8)

{
  undefined *puVar1;
  uint uVar2;
  uint uVar3;
  
  puVar1 = PTR_if_ctrl_000103ac;
  uVar2 = (uint)param_2;
  param_1 = param_1 & 0xff;
  uVar3 = (uint)param_8;
  if (param_5 < 6) {
    if (uVar2 == 0) {
      PTR_if_ctrl_000103ac[param_1 * 0x10 + 0xc] = param_2;
    }
    puVar1[param_1 * 0x10 + 0xd] = param_2;
  }
  else {
    PTR_if_ctrl_000103ac[param_1 * 0x10 + 0xc] = param_2;
  }
  (*(code *)PTR_wDev_Insert_KeyEntry_000103b0)(uVar2,param_1,param_3);
  (*(code *)PTR_wDev_Crypto_Conf_000103b4)(param_1,uVar2,uVar3);
  return;
}

