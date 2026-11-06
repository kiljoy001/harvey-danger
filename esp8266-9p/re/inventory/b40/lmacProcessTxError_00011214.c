
void lmacProcessTxError(uint param_1,uint param_2)

{
  undefined *puVar1;
  
  param_1 = param_1 & 0xff;
  if (param_1 < 0x12) {
    if (param_1 < 0x10) {
      if ((param_1 == 1) || (param_1 == 3)) {
        (*(code *)PTR_lmacProcessAckTimeout_00011204)();
        return;
      }
LAB_00011264:
      (*(code *)PTR_esp_log_write_0001120c)(1,PTR_DAT_000111f8,PTR_s_tx_error_0x_x_000111f4);
      (*(code *)PTR_ets_printf_00011210)(PTR_s__s__u_00011200,PTR_DAT_000111fc,0x53a);
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
  }
  else if (1 < param_1 - 0xc0) goto LAB_00011264;
  puVar1 = PTR_DAT_000111f0 + (param_2 & 0xff) * 0x24;
  puVar1[0x11] = 6;
  (*(code *)PTR_FUN_00011208)(puVar1,0);
  return;
}

