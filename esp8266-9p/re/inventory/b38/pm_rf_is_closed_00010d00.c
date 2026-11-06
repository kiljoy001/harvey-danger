
bool pm_rf_is_closed(void)

{
  return *(int *)(PTR_DAT_00010cfc + 0x6c) == 1 || *(int *)(PTR_DAT_00010cfc + 0x6c) == 6;
}

