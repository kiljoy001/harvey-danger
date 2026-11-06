
void ic_set_ptk_alg(uint param_1,undefined1 param_2)

{
  PTR_if_ctrl_00010180[(param_1 & 0xff) * 0x10 + 0xc] = param_2;
  return;
}

