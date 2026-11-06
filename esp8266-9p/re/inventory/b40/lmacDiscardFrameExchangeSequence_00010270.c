
void FUN_00010270(int *param_1)

{
  int iVar1;
  int iVar2;
  
  if (*(char *)((int)param_1 + 0x11) != '\x06') {
    (*(code *)PTR_ets_printf_00010268)(PTR_s__s__u_0001025c,PTR_DAT_00010258,0x340);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  iVar2 = *param_1;
  *(undefined1 *)((int)param_1 + 0x11) = 0;
  *param_1 = 0;
  iVar1 = *(int *)(iVar2 + 0x28);
  *(uint *)(iVar1 + 4) = *(uint *)(iVar1 + 4) & DAT_00010260 | DAT_00010264;
  (*(code *)PTR_FUN_0001026c)(iVar2,1);
  return;
}

