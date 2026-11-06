
char ic_get_rssi(undefined1 param_1)

{
  char cVar1;
  int iVar2;
  
  iVar2 = (*(code *)PTR_rc_get_trc_00010458)(param_1);
  cVar1 = '\x1f';
  if (iVar2 != 0) {
    cVar1 = *(char *)(iVar2 + 3) + -0x60;
  }
  return cVar1;
}

