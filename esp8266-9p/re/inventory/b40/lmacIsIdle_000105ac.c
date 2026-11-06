
bool lmacIsIdle(uint param_1)

{
  return PTR_DAT_000105a8[(param_1 & 0xff) * 0x24 + 0x11] == '\0';
}

