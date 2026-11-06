
undefined1 ic_disable_interface(uint param_1)

{
  undefined1 uVar1;
  undefined *puVar2;
  
  puVar2 = PTR_interface_mask_000100f0;
  if (((uint)(byte)*PTR_interface_mask_000100f0 & 1 << ((byte)param_1 & 0x1f)) != 0) {
    *PTR_interface_mask_000100f0 =
         *PTR_interface_mask_000100f0 & (byte)(0xfffffffefffffffe >> 0x20 - (param_1 & 0x1f));
    (*(code *)PTR_rc_disable_trc_by_interface_000100f4)();
  }
  uVar1 = (*(code *)PTR_bit_popcount_000100f8)(*puVar2);
  return uVar1;
}

