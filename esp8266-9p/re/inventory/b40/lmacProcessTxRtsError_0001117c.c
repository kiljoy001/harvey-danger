
void lmacProcessTxRtsError(byte param_1,uint param_2)

{
  undefined *puVar1;
  
  if (param_1 < 0x12) {
    if (param_1 < 0x10) {
      if ((param_1 == 1) || (param_1 == 3)) {
        (*(code *)PTR_lmacProcessCtsTimeout_0001116c)();
        return;
      }
LAB_000111c7:
      (*(code *)PTR_esp_log_write_00011174)(1,PTR_DAT_00011160,PTR_s_tx_rts_error_0x_x_0001115c);
      (*(code *)PTR_ets_printf_00011178)(PTR_s__s__u_00011168,PTR_DAT_00011164,0x516);
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
  }
  else if (param_1 != 0xc0) goto LAB_000111c7;
  puVar1 = PTR_DAT_00011158 + (param_2 & 0xff) * 0x24;
  puVar1[0x11] = 6;
  (*(code *)PTR_FUN_00011170)(puVar1,0);
  return;
}

