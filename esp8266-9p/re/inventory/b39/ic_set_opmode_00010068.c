
void ic_set_opmode(uint param_1,undefined4 param_2)

{
  *(undefined4 *)(PTR_if_ctrl_00010064 + (param_1 & 0xff) * 0x10 + 8) = param_2;
  return;
}

