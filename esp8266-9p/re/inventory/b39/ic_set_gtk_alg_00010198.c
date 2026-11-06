
void ic_set_gtk_alg(uint param_1,undefined1 param_2)

{
  PTR_if_ctrl_00010194[(param_1 & 0xff) * 0x10 + 0xd] = param_2;
  return;
}

