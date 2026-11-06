
void FUN_00010bc0(int *param_1,int param_2)

{
  uint uVar1;
  int iVar2;
  uint *puVar3;
  uint uVar4;
  int iVar5;
  uint uVar6;
  
  if (param_2 != 0) {
    if (1 < (byte)(*(char *)((int)param_1 + 0x11) - 1U)) {
      (*(code *)PTR_ets_printf_00010b9c)(PTR_s__s__u_00010b80,PTR_DAT_00010b7c,0x3bb);
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
    *(undefined1 *)((int)param_1 + 0x11) = 3;
    iVar2 = *param_1;
    puVar3 = *(uint **)(iVar2 + 0x28);
    if ((*puVar3 & 0x20000) == 0) {
      if (*(char *)((int)param_1 + 0xb) == '\0') {
        if (*(char *)((int)param_1 + 0xd) != '\0') {
          *(undefined1 *)((int)param_1 + 0xd) = 0;
          *(undefined1 *)((int)param_1 + 0xb) = 0;
        }
        (*(code *)PTR_rcGetRate_00010bb0)
                  (*(undefined4 *)(iVar2 + 0x20),*(undefined4 *)(iVar2 + 0x28));
        (*(code *)PTR_lmacTxFrame_00010bb4)(*param_1,(char)param_1[1]);
      }
      else {
        uVar1 = (uint)(byte)puVar3[3];
        *(ushort *)((int)param_1 + 0x16) = (ushort)(puVar3[2] >> 0x17);
        (*(code *)PTR_rcGetRate_00010ba0)
                  (*(undefined4 *)(iVar2 + 0x20),*(undefined4 *)(iVar2 + 0x28));
        if (*(byte *)(*(int *)(*param_1 + 0x28) + 0xc) != uVar1) {
          (*(code *)PTR_ppCalFrameTimes_00010ba4)();
        }
        *(ushort *)((int)param_1 + 0x16) =
             (*(short *)((int)param_1 + 0x16) + *(short *)(*(int *)(*param_1 + 0x28) + 0xe)) -
             (ushort)((uint)*(undefined4 *)(*(int *)(*param_1 + 0x28) + 8) >> 0x17);
        (*(code *)PTR_FUN_00010ba8)(param_1);
        *(undefined1 *)((int)param_1 + 0x11) = 1;
        (*(code *)PTR_wDev_EnableTransmit_00010bac)((char)param_1[1],0);
      }
    }
    else {
      *param_1 = 0;
      *(undefined1 *)((int)param_1 + 0x11) = 0;
      *(uint *)(*(int *)(iVar2 + 0x28) + 4) =
           *(uint *)(*(int *)(iVar2 + 0x28) + 4) & DAT_00010b68 | DAT_00010b84;
      (*(code *)PTR_FUN_00010bb8)(iVar2,1);
    }
    *(undefined1 *)((int)param_1 + 0x12) = 5;
    goto LAB_00010daa;
  }
  if (*(char *)((int)param_1 + 0x11) != '\x06') {
    if (*(char *)((int)param_1 + 0x11) == '\x05') {
      *(undefined1 *)((int)param_1 + 0x11) = 0;
      if (*param_1 == 0) {
        (*(code *)PTR_ets_printf_00010b94)(PTR_s__s__u_00010b80,PTR_DAT_00010b7c,0x3ac);
        do {
                    /* WARNING: Do nothing block with infinite loop */
        } while( true );
      }
      if (*(char *)((int)param_1 + 0xd) != '\0') {
        *(undefined1 *)((int)param_1 + 0xd) = 0;
        *(undefined1 *)((int)param_1 + 0xb) = 0;
      }
      if (*(char *)((int)param_1 + 0xb) != '\0') {
        *(undefined1 *)((int)param_1 + 0xb) = 0;
      }
      *(undefined1 *)((int)param_1 + 0x12) = 3;
      (*(code *)PTR_lmacRecycleMPDU_00010b98)(param_1,*param_1,1);
    }
    goto LAB_00010daa;
  }
  *(undefined1 *)((int)param_1 + 0x11) = 0;
  uVar1 = (*(code *)PTR_rcReachRetryLimit_00010b88)(*(undefined4 *)(*param_1 + 0x28));
  iVar2 = *param_1;
  if (uVar1 == 0) {
    iVar5 = *(int *)(iVar2 + 0x28);
    if ((uint)(byte)PTR_lmacConfMib_00010b64[0xd] <= (*(uint *)(iVar5 + 8) >> 8 & 0x3f))
    goto LAB_00010bfc;
    if ((*(uint *)(iVar5 + 8) >> 0xe & 0x3f) < (uint)(byte)PTR_lmacConfMib_00010b64[0xc]) {
      iVar5 = (*(code *)PTR_lmacMSDUAged_00010b90)();
      iVar2 = *param_1;
      if (iVar5 == 0) {
        iVar5 = *(int *)(iVar2 + 0x28);
        uVar6 = (uint)(*(char *)((int)param_1 + 0x11) == '\0');
        uVar4 = *(uint *)(iVar5 + 4) & DAT_00010b68;
        uVar1 = DAT_00010b78;
      }
      else {
        iVar5 = *(int *)(iVar2 + 0x28);
        uVar6 = 1;
        if (*(char *)((int)param_1 + 0x11) != '\0') {
          uVar6 = uVar1;
        }
        uVar4 = *(uint *)(iVar5 + 4) & DAT_00010b68;
        uVar1 = DAT_00010b74;
      }
    }
    else {
      uVar6 = uVar1;
      if (*(char *)((int)param_1 + 0x11) == '\0') {
        uVar6 = 1;
      }
      uVar4 = *(uint *)(iVar5 + 4) & DAT_00010b68;
      uVar1 = DAT_00010b70;
    }
    *(uint *)(iVar5 + 4) = uVar4 | uVar1;
    uVar6 = uVar6 & 0xff;
  }
  else {
LAB_00010bfc:
    uVar6 = (uint)(*(char *)((int)param_1 + 0x11) == '\0');
    *(uint *)(*(int *)(iVar2 + 0x28) + 4) =
         *(uint *)(*(int *)(iVar2 + 0x28) + 4) & DAT_00010b68 | DAT_00010b6c;
  }
  (*(code *)PTR_FUN_00010b8c)(iVar2,uVar6);
LAB_00010daa:
  if (*(char *)((int)param_1 + 0xd) != '\0') {
    (*(code *)PTR_ets_printf_00010bbc)(PTR_s__s__u_00010b80,PTR_DAT_00010b7c,0x3c2);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  return;
}

