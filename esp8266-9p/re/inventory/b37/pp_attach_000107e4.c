
void pp_attach(void)

{
  char cVar1;
  undefined2 uVar2;
  undefined4 *puVar3;
  undefined4 uVar4;
  int iVar5;
  uint uVar6;
  uint uVar7;
  undefined4 *puVar8;
  int iVar9;
  undefined *puVar10;
  undefined1 uVar11;
  undefined2 *puVar12;
  uint uVar13;
  
  puVar3 = *(undefined4 **)PTR_pTxRx_0001074c;
  puVar8 = puVar3 + 6;
  do {
    *puVar8 = 0;
    puVar8[1] = puVar8;
    *(undefined1 *)((int)puVar8 + 0xb) = 0;
    puVar8 = puVar8 + 4;
    iVar9 = 0;
  } while (puVar8 != puVar3 + 0x26);
  *(undefined1 *)(puVar3 + 8) = 7;
  *(undefined1 *)(puVar3 + 0xc) = 7;
  *(undefined1 *)(puVar3 + 5) = 2;
  *(undefined1 *)((int)puVar3 + 0x15) = 3;
  *(undefined1 *)((int)puVar3 + 0x16) = 5;
  *(undefined1 *)((int)puVar3 + 0x17) = 6;
  puVar3[0x27] = puVar3 + 0x26;
  puVar3[0x29] = puVar3 + 0x28;
  puVar3[0x2b] = puVar3 + 0x2a;
  uVar11 = 1;
  *puVar3 = 0;
  puVar3[0x2d] = puVar3 + 0x2c;
  *(undefined1 *)((int)puVar3 + 0x23) = 1;
  uVar4 = DAT_00010754;
  *(undefined1 *)((int)puVar3 + 0x33) = 1;
  puVar3[1] = 0;
  puVar3[2] = 0;
  puVar3[3] = 0;
  puVar3[4] = 0;
  puVar3[0x26] = 0;
  puVar3[0x28] = 0;
  puVar3[0x2a] = 0;
  puVar3[0x2c] = 0;
  memw();
  *DAT_00010750 = uVar4;
  (*(code *)PTR_esf_buf_setup_00010794)();
  uVar4 = (*(code *)PTR___wifi_queue_create_00010798)(0x4b,8);
  *(undefined4 *)PTR_DAT_00010758 = uVar4;
  iVar5 = (*(code *)PTR___wifi_task_get_max_priority_0001079c)();
  (*(code *)PTR___wifi_task_create_000107a0)
            (PTR_ppTask_00010764,PTR_DAT_00010760,
             *(undefined4 *)PTR__g_esp_wifi_ppt_task_stk_size_0001075c,iVar9,iVar5 + -2);
  (*(code *)PTR___wifi_task_get_max_priority_000107a4)();
  puVar10 = PTR_DAT_0001076c;
  (*(code *)PTR_os_timer_setfn_000107a8)(PTR_DAT_0001076c,PTR_pp_noise_test_00010768,iVar9);
  (*(code *)PTR___wifi_task_suspend_all_000107ac)();
  (*(code *)PTR_os_timer_disarm_000107b0)(puVar10);
  iVar5 = (*(code *)PTR_pm_rf_is_closed_000107b4)();
  if (iVar5 == iVar9) {
    (*(code *)PTR_noise_check_loop_000107b8)(1);
  }
  else {
    *PTR_pend_flag_noise_check_00010770 = uVar11;
  }
  uVar6 = (*(code *)PTR_RxNodeNum_000107bc)();
  uVar13 = uVar6 & 0xff;
  uVar7 = (*(code *)PTR_TxNodeNum_000107c0)();
  puVar12 = (undefined2 *)PTR_NoiseTimerInterval_00010778;
  puVar10 = PTR_DAT_00010774;
  if ((uVar13 < 3) || (uVar7 < 2)) {
    cVar1 = *PTR_DAT_00010774;
    uVar2 = *(undefined2 *)PTR_NoiseTimerInterval_00010778;
    *PTR_DAT_00010774 = cVar1 + 1U;
    iVar9 = (*(code *)PTR___divsi3_000107c4)(1000,uVar2);
    if (iVar9 <= (int)(uint)(byte)(cVar1 + 1U)) {
      *puVar10 = 0;
      if (uVar13 < 3) {
        (*(code *)PTR_esp_log_write_000107c8)
                  (1,PTR_DAT_00010780,PTR_s_LmacRxBlk__d_0001077c,uVar6 & 0xff);
      }
      if (uVar7 < 2) {
        (*(code *)PTR_esp_log_write_000107cc)(1,PTR_DAT_00010780,PTR_s_LmacTxBlk__d_00010784,uVar7);
      }
    }
  }
  else {
    *PTR_DAT_00010774 = 0;
  }
  iVar9 = (*(code *)PTR_read_hw_noisefloor_000107d0)();
  puVar10 = PTR_DAT_0001078c;
  cVar1 = *PTR_DAT_0001078c;
  *PTR_DAT_00010788 = (char)(iVar9 + 2 >> 2);
  uVar4 = DAT_00010790;
  uVar6 = (uint)(byte)(cVar1 + 1U);
  uVar2 = *puVar12;
  *puVar10 = cVar1 + 1U;
  iVar9 = (*(code *)PTR___divsi3_000107d4)(uVar4,uVar2);
  if (iVar9 < (int)uVar6) {
    *puVar10 = 0;
    (*(code *)PTR_lmac_check_ac_000107d8)();
  }
  (*(code *)PTR_os_timer_arm_000107dc)(PTR_DAT_0001076c,*puVar12,0);
  (*(code *)PTR___wifi_task_resume_all_000107e0)();
  return;
}

