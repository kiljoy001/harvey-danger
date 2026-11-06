
void lmacProcessCtsTimeout(uint param_1)

{
  int iVar1;
  
  iVar1 = *(int *)(PTR_DAT_00010fa8 + (param_1 & 0xff) * 0x24);
  if (iVar1 == 0) {
    (*(code *)PTR_ets_printf_00010fb4)(PTR_s__s__u_00010fb0,PTR_DAT_00010fac,0x4e0);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  (*(code *)PTR_FUN_00010fb8)(PTR_DAT_00010fa8 + (param_1 & 0xff) * 0x24,iVar1,1);
  return;
}

