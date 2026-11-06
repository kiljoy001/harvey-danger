
void lmacClrAc(void)

{
  int iVar1;
  undefined4 *puVar2;
  int iVar3;
  
  puVar2 = (undefined4 *)PTR_DAT_000112b4;
  if (*(int *)PTR_DAT_000112b4 != 0) {
    (*(code *)PTR_lmacRecycleMPDU_000112bc)(PTR_DAT_000112b4,*(int *)PTR_DAT_000112b4,0);
    *puVar2 = 0;
  }
  iVar3 = 0x33;
  do {
    iVar1 = (*(code *)PTR_ppFetchTxQFirstAvail_000112c4)(0);
    if (iVar1 == 0) break;
    iVar3 = iVar3 + -1;
    (*(code *)PTR_lmacRecycleMPDU_000112c0)(puVar2,iVar1,0);
  } while (iVar3 != 0);
  *(undefined1 *)((int)puVar2 + 0x11) = 0;
  if (puVar2[0x12] != 0) {
    (*(code *)PTR_lmacRecycleMPDU_000112c8)(PTR_DAT_000112b8,puVar2[0x12],1);
    puVar2[0x12] = 0;
  }
  iVar3 = 0x33;
  do {
    iVar1 = (*(code *)PTR_ppFetchTxQFirstAvail_000112d0)(2);
    if (iVar1 == 0) break;
    iVar3 = iVar3 + -1;
    (*(code *)PTR_lmacRecycleMPDU_000112cc)(PTR_DAT_000112b8,iVar1,0);
  } while (iVar3 != 0);
  *(undefined1 *)((int)puVar2 + 0x59) = 0;
  return;
}

