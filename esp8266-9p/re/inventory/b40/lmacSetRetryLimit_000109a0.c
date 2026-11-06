
void lmacSetRetryLimit(byte param_1)

{
  undefined *puVar1;
  
  puVar1 = PTR_lmacConfMib_0001099c;
  if (param_1 < 0x15) {
    if (param_1 < 5) {
      param_1 = 5;
    }
  }
  else {
    param_1 = 0x14;
  }
  PTR_lmacConfMib_0001099c[0xd] = param_1;
  puVar1[0xc] = param_1;
  return;
}

