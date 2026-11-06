
void FUN_00010010(int param_1)

{
  undefined *puVar1;
  
  puVar1 = PTR_if_ctrl_00010004;
  if (PTR_g_ic_00010000[0x160] == '\0') {
    PTR_if_ctrl_00010004[param_1 * 0x10 + 0xc] = 0;
    puVar1[param_1 * 0x10 + 0xd] = 0;
    (*(code *)PTR_wDev_Crypto_Disable_00010008)(param_1);
  }
  (*(code *)PTR_wDev_SetRxPolicy_0001000c)(1,param_1,0);
  return;
}

