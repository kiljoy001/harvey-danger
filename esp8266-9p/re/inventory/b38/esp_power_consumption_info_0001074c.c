
void esp_power_consumption_info(uint param_1)

{
  bool bVar1;
  undefined *puVar2;
  int iVar3;
  undefined4 uVar4;
  undefined4 uVar5;
  undefined4 uVar6;
  uint uVar7;
  int iVar8;
  uint uVar9;
  uint uVar10;
  uint uVar11;
  uint uVar12;
  uint uVar13;
  uint uVar14;
  uint uVar15;
  undefined *puVar16;
  undefined8 uVar17;
  longlong lVar18;
  longlong lVar19;
  int local_40;
  uint uStack_3c;
  int iStack_38;
  uint uStack_34;
  int iStack_30;
  
  param_1 = param_1 & 0xff;
  uVar17 = (*(code *)PTR_esp_timer_get_time_00010678)();
  uStack_3c = (uint)((ulonglong)uVar17 >> 0x20);
  uVar10 = (uint)uVar17;
  uVar14 = *(uint *)PTR_DAT_000105c4;
  local_40 = *(int *)(PTR_DAT_000105c4 + 4);
  uVar9 = uVar10;
  if ((*(int *)(PTR_DAT_000105c8 + 0x6c) != 1) && (*(int *)(PTR_DAT_000105c8 + 0x6c) != 6)) {
    uVar17 = (*(code *)PTR_esp_timer_get_time_0001067c)();
    bVar1 = uVar14 < uVar14 - *(int *)PTR_DAT_000105cc;
    uVar14 = (uint)uVar17 + (uVar14 - *(int *)PTR_DAT_000105cc);
    local_40 = (uint)(uVar14 < (uint)uVar17) +
               (int)((ulonglong)uVar17 >> 0x20) +
               ((local_40 - *(int *)(PTR_DAT_000105cc + 4)) - (uint)bVar1);
  }
  puVar16 = PTR_DAT_000105d4;
  uVar15 = *(uint *)PTR_WdevTimOffSet_000105d0;
  iStack_38 = *(int *)(PTR_WdevTimOffSet_000105d0 + 4);
  uVar7 = uVar10 - uVar15;
  uVar11 = uVar7 - uVar14;
  iVar3 = (((uStack_3c - iStack_38) - (uint)(uVar9 < uVar7)) - local_40) - (uint)(uVar7 < uVar11);
  uVar7 = uVar14;
  uVar12 = uVar9;
  uVar13 = uVar15;
  uStack_34 = uVar11;
  iStack_30 = iVar3;
  if (param_1 != 0) {
    iVar8 = *(int *)(PTR_DAT_000105d4 + 4);
    uVar7 = uVar14 - *(int *)PTR_DAT_000105d4;
    *(uint *)PTR_DAT_000105d4 = uVar14;
    *(int *)(puVar16 + 4) = local_40;
    puVar16 = PTR_DAT_000105d8;
    local_40 = (local_40 - iVar8) - (uint)(uVar14 < uVar7);
    uVar13 = uVar15 - *(int *)PTR_DAT_000105d8;
    iVar8 = *(int *)(PTR_DAT_000105d8 + 4);
    *(uint *)PTR_DAT_000105d8 = uVar15;
    *(int *)(puVar16 + 4) = iStack_38;
    puVar16 = PTR_DAT_000105dc;
    iStack_38 = (iStack_38 - iVar8) - (uint)(uVar15 < uVar13);
    iVar8 = *(int *)(PTR_DAT_000105dc + 4);
    uVar12 = uVar10 - *(int *)PTR_DAT_000105dc;
    *(uint *)(PTR_DAT_000105dc + 4) = uStack_3c;
    puVar2 = PTR_DAT_000105e0;
    *(uint *)puVar16 = uVar10;
    uStack_3c = (uStack_3c - iVar8) - (uint)(uVar9 < uVar12);
    uStack_34 = uVar11 - *(int *)puVar2;
    iStack_30 = (iVar3 - *(int *)(puVar2 + 4)) - (uint)(uVar11 < uStack_34);
    *(uint *)puVar2 = uVar11;
    *(int *)(puVar2 + 4) = iVar3;
  }
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_00010680)();
  uVar5 = (*(code *)PTR___udivdi3_00010684)
                    (uVar12 + 500,(uVar12 + 500 < uVar12) + uStack_3c,DAT_000105e4,DAT_000105e8);
  (*(code *)PTR_ets_printf_00010688)(PTR_DAT_000105f0,uVar4,PTR_DAT_000105ec,uStack_3c,uVar12,uVar5)
  ;
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_0001068c)();
  uVar5 = (*(code *)PTR___udivdi3_00010690)
                    (uVar7 + 500,(uint)(uVar7 + 500 < uVar7) + local_40,DAT_000105e4,DAT_000105e8);
  (*(code *)PTR_ets_printf_00010694)(PTR_DAT_000105f4,uVar4,PTR_DAT_000105ec,local_40,uVar7,uVar5);
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_00010698)();
  uVar5 = (*(code *)PTR___udivdi3_0001069c)
                    (uStack_34 + 500,(uint)(uStack_34 + 500 < uStack_34) + iStack_30,DAT_000105e4,
                     DAT_000105e8);
  (*(code *)PTR_ets_printf_000106a0)
            (PTR_DAT_000105f8,uVar4,PTR_DAT_000105ec,iStack_30,uStack_34,uVar5);
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_000106a4)();
  uVar5 = (*(code *)PTR___udivdi3_000106a8)
                    (uVar13 + 500,(uint)(uVar13 + 500 < uVar13) + iStack_38,DAT_000105e4,
                     DAT_000105e8);
  (*(code *)PTR_ets_printf_000106ac)(PTR_DAT_000105fc,uVar4,PTR_DAT_000105ec,iStack_38,uVar13,uVar5)
  ;
  uVar17 = (*(code *)PTR___muldi3_000106b0)(uVar7,local_40,DAT_00010600,DAT_00010604);
  lVar18 = (*(code *)PTR___udivdi3_000106b4)
                     ((int)uVar17,(int)((ulonglong)uVar17 >> 0x20),uVar12,uStack_3c);
  uVar4 = (*(code *)PTR___udivdi3_000106b8)
                    ((int)(lVar18 + 5),(int)((ulonglong)(lVar18 + 5) >> 0x20),DAT_00010608,
                     DAT_0001060c);
  uVar5 = (*(code *)PTR_esp_log_early_timestamp_000106bc)();
  uVar6 = (*(code *)PTR___umodsi3_000106c0)(uVar4,100);
  uVar4 = (*(code *)PTR___udivsi3_000106c4)(uVar4,100);
  (*(code *)PTR_ets_printf_000106c8)(PTR_DAT_00010610,uVar5,PTR_DAT_000105ec,uVar4,uVar6,0x25);
  uVar17 = (*(code *)PTR___muldi3_000106cc)(uStack_34,iStack_30,DAT_00010600,DAT_00010604);
  lVar18 = (*(code *)PTR___udivdi3_000106d0)
                     ((int)uVar17,(int)((ulonglong)uVar17 >> 0x20),uVar12,uStack_3c);
  uVar4 = (*(code *)PTR___udivdi3_000106d4)
                    ((int)(lVar18 + 5),(int)((ulonglong)(lVar18 + 5) >> 0x20),DAT_00010608,
                     DAT_0001060c);
  uVar5 = (*(code *)PTR_esp_log_early_timestamp_000106d8)();
  uVar6 = (*(code *)PTR___umodsi3_000106dc)(uVar4,100);
  uVar4 = (*(code *)PTR___udivsi3_000106e0)(uVar4,100);
  (*(code *)PTR_ets_printf_000106e4)(PTR_DAT_00010614,uVar5,PTR_DAT_000105ec,uVar4,uVar6,0x25);
  uVar17 = (*(code *)PTR___muldi3_000106e8)(uVar13,iStack_38,DAT_00010600,DAT_00010604);
  lVar18 = (*(code *)PTR___udivdi3_000106ec)
                     ((int)uVar17,(int)((ulonglong)uVar17 >> 0x20),uVar12,uStack_3c);
  uVar4 = (*(code *)PTR___udivdi3_000106f0)
                    ((int)(lVar18 + 5),(int)((ulonglong)(lVar18 + 5) >> 0x20),DAT_00010608,
                     DAT_0001060c);
  uVar5 = (*(code *)PTR_esp_log_early_timestamp_000106f4)();
  uVar6 = (*(code *)PTR___umodsi3_000106f8)(uVar4,100);
  uVar4 = (*(code *)PTR___udivsi3_000106fc)(uVar4,100);
  (*(code *)PTR_ets_printf_00010700)(PTR_DAT_00010618,uVar5,PTR_DAT_000105ec,uVar4,uVar6,0x25);
  uVar17 = (*(code *)PTR___muldi3_00010704)(uVar13,iStack_38,DAT_0001061c,DAT_00010620);
  uVar4 = (undefined4)uVar17;
  lVar18 = (*(code *)PTR___muldi3_00010708)(uStack_34,iStack_30,DAT_00010624,DAT_00010628);
  lVar18 = lVar18 + CONCAT44((int)((ulonglong)uVar17 >> 0x20),uVar4) + (ulonglong)DAT_0001062c;
  uVar9 = (uint)lVar18;
  iVar3 = (int)((ulonglong)lVar18 >> 0x20);
  uVar17 = (*(code *)PTR___muldi3_0001070c)(uVar7,local_40,DAT_00010634,DAT_00010638);
  uVar10 = uVar9 + (int)uVar17;
  uVar9 = (uint)(uVar10 < uVar9);
  iVar3 = iVar3 + (int)((ulonglong)uVar17 >> 0x20);
  iVar8 = uVar9 + iVar3;
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_00010710)(uVar9,iVar3);
  uVar17 = (*(code *)PTR___udivdi3_00010714)(uVar10,iVar8,DAT_0001063c,DAT_00010640);
  uVar5 = (*(code *)PTR___umoddi3_00010718)
                    ((int)uVar17,(int)((ulonglong)uVar17 >> 0x20),DAT_000105e4,DAT_000105e8);
  uVar6 = (*(code *)PTR___udivdi3_0001071c)(uVar10,iVar8,DAT_00010644,DAT_00010648);
  (*(code *)PTR_ets_printf_00010720)(PTR_DAT_0001064c,uVar4,PTR_DAT_000105ec,uVar6,uVar5);
  uVar17 = (*(code *)PTR___muldi3_00010724)(uVar13,iStack_38,DAT_00010650,DAT_00010654);
  uVar4 = (undefined4)((ulonglong)uVar17 >> 0x20);
  lVar18 = (*(code *)PTR___muldi3_00010728)(uStack_34,iStack_30,DAT_00010658,DAT_0001065c);
  lVar18 = lVar18 + CONCAT44(uVar4,(int)uVar17);
  uVar4 = (undefined4)((ulonglong)lVar18 >> 0x20);
  puVar16 = PTR_DAT_000105ec;
  lVar19 = (*(code *)PTR___muldi3_0001072c)(uVar7,local_40,DAT_00010660,DAT_00010664);
  lVar19 = lVar19 + CONCAT44(uVar4,(int)lVar18);
  uVar10 = (uint)lVar19;
  uVar9 = uVar10 + (uStack_3c << 0x1f | uVar12 >> 1);
  uVar17 = (*(code *)PTR___udivdi3_00010730)
                     (uVar9,(uint)(uVar9 < uVar10) +
                            (int)((ulonglong)lVar19 >> 0x20) + (uStack_3c >> 1),uVar12,uStack_3c);
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_00010734)();
  uVar5 = (*(code *)PTR___umoddi3_00010738)
                    ((int)uVar17,(int)((ulonglong)uVar17 >> 0x20),DAT_000105e4,DAT_000105e8);
  uVar6 = (*(code *)PTR___udivdi3_0001073c)
                    ((int)uVar17,(int)((ulonglong)uVar17 >> 0x20),DAT_000105e4,DAT_000105e8);
  (*(code *)PTR_ets_printf_00010740)(PTR_DAT_00010668,uVar4,puVar16,uVar6,uVar5);
  uVar4 = (*(code *)PTR_esp_log_early_timestamp_00010744)();
  (*(code *)PTR_ets_printf_00010748)
            (PTR_DAT_00010674,uVar4,puVar16,*(undefined4 *)PTR_DAT_00010670,
             *(undefined4 *)PTR_DAT_0001066c);
  return;
}

