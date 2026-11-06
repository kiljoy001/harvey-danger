
undefined1 ic_enable_interface(uint param_1,undefined4 param_2,undefined4 param_3)

{
  undefined *puVar1;
  undefined1 uVar2;
  int iVar3;
  
  param_1 = param_1 & 0xff;
  iVar3 = (*(code *)PTR_memcpy_00010080)(PTR_if_ctrl_00010078 + param_1 * 0x10,param_2,6);
  puVar1 = PTR_interface_mask_0001007c;
  *(undefined4 *)(iVar3 + 8) = param_3;
  *puVar1 = (byte)(1 << 0x20 - (0x20 - (param_1 & 0x1f))) | *puVar1;
  uVar2 = (*(code *)PTR_bit_popcount_00010084)();
  return uVar2;
}

