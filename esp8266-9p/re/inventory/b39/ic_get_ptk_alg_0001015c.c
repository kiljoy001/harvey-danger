
undefined1 ic_get_ptk_alg(uint param_1)

{
  return PTR_if_ctrl_00010158[(param_1 & 0xff) * 0x10 + 0xc];
}

