
byte ic_interface_enabled(uint param_1)

{
  return (byte)*PTR_interface_mask_000100d8 >> (param_1 & 0x1f) & 1;
}

