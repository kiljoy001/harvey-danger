
void dbg_lmac_show(void)

{
  int iVar1;
  int iVar2;
  int *piVar3;
  int iVar4;
  undefined *puVar5;
  int iVar6;
  
  piVar3 = (int *)PTR_pTxRx_00011650;
  (*(code *)PTR_printf_00011688)(PTR_s_T_q_en__0x_x_00011654,**(undefined4 **)PTR_pTxRx_00011650);
  iVar4 = 0;
  puVar5 = PTR_s_<0x_x,__d>_00011658;
  do {
    iVar2 = *piVar3 + iVar4;
    iVar1 = iVar4 * 4;
    iVar4 = iVar4 + 1;
    (*(code *)PTR_printf_0001168c)
              (puVar5,*(undefined4 *)(iVar1 + *piVar3 + 4),*(undefined1 *)(iVar2 + 0x14));
  } while (iVar4 != 4);
  iVar4 = 0;
  (*(code *)PTR_printf_00011690)(PTR_s_txqs__0001165c);
  do {
    (*(code *)PTR_printf_00011694)(PTR_s_PP_TXQ___d___00011660,iVar4);
    iVar1 = iVar4 + 1;
    iVar2 = 0;
    for (iVar6 = *(int *)(*piVar3 + iVar1 * 0x10 + 8); iVar6 != 0; iVar6 = *(int *)(iVar6 + 0x24)) {
      (*(code *)PTR_printf_00011698)(PTR_s___d___p_00011664,iVar2,iVar6);
      iVar2 = iVar2 + 1;
    }
    iVar4 = iVar4 << 4;
    puVar5 = PTR_DAT_00011668;
    (*(code *)PTR_printf_0001169c)(PTR_DAT_00011668);
    iVar4 = *piVar3 + iVar4;
    (*(code *)PTR_printf_000116a0)
              (PTR_s_tid__d_con__d_ifidx__d_nonqos__d_0001166c,*(undefined1 *)(iVar4 + 0x20),
               *(undefined1 *)(iVar4 + 0x21),*(undefined1 *)(iVar4 + 0x22),
               *(undefined1 *)(iVar4 + 0x23),*(undefined1 *)(iVar4 + 0x24));
    iVar4 = iVar1;
  } while (iVar1 != 8);
  (*(code *)PTR_printf_000116a4)(PTR_s_wait_txq__00011670);
  do {
    (*(code *)PTR_printf_000116a8)(PTR_s__d_wait_txq_00011674,iVar6);
    iVar4 = 0;
    for (iVar1 = *(int *)(iVar6 * 8 + *piVar3 + 0x98); iVar1 != 0; iVar1 = *(int *)(iVar1 + 0x24)) {
      (*(code *)PTR_printf_000116ac)(PTR_s___d___p_00011664,iVar4,iVar1);
      iVar4 = iVar4 + 1;
    }
    iVar6 = iVar6 + 1;
    (*(code *)PTR_printf_000116b0)(puVar5);
  } while (iVar6 != 2);
  (*(code *)PTR_printf_000116b4)(PTR_s_done_q__00011678);
  for (iVar4 = *(int *)(*piVar3 + 0xa8); iVar4 != 0; iVar4 = *(int *)(iVar4 + 0x24)) {
    (*(code *)PTR_printf_000116b8)(PTR_s___d___p_00011664,iVar1,iVar4);
    iVar1 = iVar1 + 1;
  }
  (*(code *)PTR_printf_000116bc)(puVar5);
  (*(code *)PTR_printf_000116c0)(PTR_s_rx_q__0001167c);
  for (iVar1 = *(int *)(*piVar3 + 0xb0); iVar1 != 0; iVar1 = *(int *)(iVar1 + 0x24)) {
    (*(code *)PTR_printf_000116c4)(PTR_s___d___p_00011664,iVar4,iVar1);
    iVar4 = iVar4 + 1;
  }
  (*(code *)PTR_printf_000116c8)(puVar5);
  (*(code *)PTR_printf_000116cc)(PTR_s_txCBmap_0x_x_00011680,*(undefined4 *)(*piVar3 + 0xb8));
  (*(code *)PTR_printf_000116d0)(PTR_s_acs__00011684);
  (*(code *)PTR_dbg_display_acs_000116d4)();
  return;
}

