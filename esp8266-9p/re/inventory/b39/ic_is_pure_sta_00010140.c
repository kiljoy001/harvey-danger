
bool ic_is_pure_sta(uint param_1)

{
  return *(int *)(PTR_if_ctrl_0001013c + (param_1 & 0xff) * 0x10 + 8) != 1;
}

