
void ppEnqueueTxDone(int param_1)

{
  undefined *puVar1;
  
  if (param_1 == 0) {
    (*(code *)PTR_ets_printf_000106b0)(PTR_s__s__u_000106a8,PTR_DAT_000106a4,DAT_000106a0);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  (*(code *)PTR_vPortEnterCritical_000106b4)();
  *(undefined4 *)(param_1 + 0x24) = 0;
  puVar1 = PTR_pTxRx_000106ac;
  **(int **)(*(int *)PTR_pTxRx_000106ac + 0xac) = param_1;
  *(int *)(*(int *)puVar1 + 0xac) = param_1 + 0x24;
  (*(code *)PTR_vPortExitCritical_000106b8)();
  return;
}

