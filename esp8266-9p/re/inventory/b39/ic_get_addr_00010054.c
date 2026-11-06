
undefined * ic_get_addr(uint param_1)

{
  return PTR_if_ctrl_00010050 + (param_1 & 0xff) * 0x10;
}

