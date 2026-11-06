
void pm_wait_bcn_fun(void)

{
  byte bVar1;
  undefined *puVar2;
  undefined *puVar3;
  undefined *puVar4;
  undefined4 uVar5;
  int iVar6;
  uint uVar7;
  byte bVar8;
  int iVar9;
  
  (*(code *)PTR___wifi_task_suspend_all_00011954)();
  puVar4 = PTR_DAT_00011948;
  puVar3 = PTR_DAT_00011944;
  puVar2 = PTR_DAT_00011940;
  bVar1 = PTR_DAT_00011940[0x8a];
  iVar6 = *(int *)(PTR_DAT_00011940 + 0x58) + *(int *)(PTR_DAT_00011940 + 0x54);
  *(int *)(PTR_DAT_00011940 + 0x60) = *(int *)(PTR_DAT_00011940 + 0x58);
  *(int *)(puVar2 + 0x58) = iVar6;
  *(int *)(puVar2 + 0x5c) = iVar6;
  iVar9 = *(int *)puVar3;
  iVar6 = *(int *)puVar4;
  bVar8 = puVar2[0x82] + 1;
  puVar2[0x82] = bVar8;
  *(uint *)puVar3 = iVar9 + 0x100U;
  *(uint *)puVar4 = iVar6 + 0x200U;
  uVar7 = (uint)bVar1 * 1000 + 32000;
  if (uVar7 < iVar9 + 0x100U) {
    *(uint *)puVar3 = uVar7;
  }
  if (*(uint *)puVar3 < 0x5dc) {
    *(undefined4 *)puVar3 = 0x5dc;
  }
  if ((uint)(byte)puVar2[0x8b] * 1000 < iVar6 + 0x200U) {
    *(uint *)puVar4 = (uint)(byte)puVar2[0x8b] * 1000;
  }
  if (*(uint *)puVar4 <= DAT_0001194c) {
    *(undefined4 *)puVar4 = DAT_00011950;
  }
  if ((uint)(byte)puVar2[0x8c] < (uint)bVar8) {
    (*(code *)PTR___wifi_task_suspend_all_00011958)();
    uVar5 = 4;
  }
  else if ((byte)puVar2[0x8c] + 1 < (uint)bVar8) {
    (*(code *)PTR___wifi_task_suspend_all_0001195c)();
    uVar5 = 2;
  }
  else {
    (*(code *)PTR___wifi_task_suspend_all_00011960)();
    uVar5 = 6;
  }
  (*(code *)PTR_pm_change_to_state_00011964)(uVar5);
  (*(code *)PTR___wifi_task_resume_all_00011968)();
  (*(code *)PTR___wifi_task_resume_all_0001196c)();
  return;
}

