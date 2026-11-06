
void pm_have_more_data_to_recv(void)

{
  undefined *puVar1;
  
  puVar1 = PTR_DAT_00010cb0;
  if ((byte)PTR_DAT_00010cb0[0x84] < (byte)PTR_DAT_00010cb0[0x88]) {
    PTR_DAT_00010cb0[0x84] = 0;
  }
  if ((byte)puVar1[0x85] < (byte)puVar1[0x89]) {
    puVar1[0x85] = 0;
  }
  return;
}

