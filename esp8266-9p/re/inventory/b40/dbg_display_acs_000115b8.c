
void dbg_display_acs(void)

{
  short *psVar1;
  short *psVar2;
  undefined2 *puVar3;
  undefined4 *puVar4;
  undefined4 *puVar5;
  undefined4 *puVar6;
  undefined4 uVar7;
  undefined4 *puVar8;
  int iVar9;
  int iVar10;
  
  (*(code *)PTR_printf_000115a8)(PTR_s_Access__00011598);
  puVar8 = (undefined4 *)PTR_DAT_00011594;
  iVar9 = 0;
  do {
    iVar10 = iVar9 + 1;
    (*(code *)PTR_printf_000115ac)
              (PTR_s__d__st__d_aifs__d_cw__d_cw_min___0001159c,iVar9,
               *(undefined1 *)((int)puVar8 + 0x11),*(undefined1 *)((int)puVar8 + 5),
               *(undefined1 *)((int)puVar8 + 6),*(undefined1 *)((int)puVar8 + 7),
               *(undefined1 *)(puVar8 + 2),*(undefined1 *)((int)puVar8 + 9),
               *(undefined1 *)((int)puVar8 + 10));
    (*(code *)PTR_printf_000115b0)
              (PTR_s_intxop__d_txop_enough__d_trytxop_000115a0,*(undefined1 *)((int)puVar8 + 0xb),
               *(undefined1 *)(puVar8 + 3),*(undefined1 *)((int)puVar8 + 0xd),
               *(undefined1 *)((int)puVar8 + 0xf),*(undefined1 *)(puVar8 + 4),
               *(undefined1 *)((int)puVar8 + 0x11),*(undefined1 *)((int)puVar8 + 0x12));
    uVar7 = *puVar8;
    puVar3 = (undefined2 *)((int)puVar8 + 0x1a);
    puVar4 = puVar8 + 6;
    psVar1 = (short *)((int)puVar8 + 0x16);
    psVar2 = (short *)(puVar8 + 5);
    puVar6 = puVar8 + 8;
    puVar5 = puVar8 + 7;
    puVar8 = puVar8 + 9;
    (*(code *)PTR_printf_000115b4)
              (PTR_s_txop__d_txop_delta__d_txop_max___000115a4,(int)*psVar2,(int)*psVar1,
               *(undefined2 *)puVar4,*puVar3,*puVar5,*puVar6,uVar7);
    iVar9 = iVar10;
  } while (iVar10 != 8);
  return;
}

