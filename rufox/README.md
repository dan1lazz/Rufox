# rufox — Rufus, портированный на Linux

**rufox** («Rufus для Linux») — полноценный порт логики [Rufus](https://rufus.ie)
(Pete Batard, GPL-3.0-or-later) на C + GTK4/libadwaita. Не «обёртка над dd»,
а перенос тех самых алгоритмов: раскладки разделов, анализа образов, патчинга
загрузочных конфигов, MBR-хирургии и правил выбора файловых систем.

Версия следует апстриму: **4.15.0** (тэг `v4.15`).

```
┌ Drive Properties ────────────────────────────────┐
│ Device           [ SanDisk Ultra (sdb)  ▼]       │
│ Boot selection   [ ubuntu-24.04.iso ▼]  [SELECT] │
│ Image option     [ Standard Windows Installation]│  ← только для Win-образов
├ Format Options ──────────────────────────────────┤
│ Partition scheme [ GPT ▼]   Target [ UEFI ▼]     │
│ File system      [ FAT32 ▼] Cluster [ 32 KB ▼]   │
│ Volume label     [ UBUNTU2404L ]                  │
├ Advanced ────────────────────────────────────────┤
│ quick format · old-BIOS fixes · bad blocks ·     │
│ persistence · Win11 bypass · no-MS-account ·     │
├ Log ─────────────────────────────────────────────┤
│ [hh:mm:ss] …                                      │
└ START / CANCEL + прогресс со скоростью и ETA ─────┘
```

Скриншот: [`docs/screenshot.png`](docs/screenshot.png).

## Сборка

```sh
sudo apt install meson ninja-build pkg-config \
    libgtk-4-dev libadwaita-1-dev libgudev-1.0-dev libudisks2-dev \
    libblkid-dev libcdio-dev libiso9660-dev libudf-dev libparted-dev \
    dosfstools mtools ntfs-3g exfatprogs e2fsprogs syslinux-common isolinux \
    xorriso grub-pc-bin grub-efi-amd64-bin
meson setup build && ninja -C build
sudo ninja -C build install
```

Тесты: `meson test -C build` (48 проверок; ISO-фикстуру собирает
`tests/make_fixture.sh`).

## Два фронта

* `rufox-gui` — GTK4/libadwaita окно, один-в-один по структуре с диалогом Rufus.
* `rufox` — CLI с теми же флагами (`-i`, `-f`, `-x` совместимы с rufus.exe):
  `list`, `info`, `plan`, `write`, `zero`, `hash`, `badblocks`.
* `rufox-helper` — привилегированная половина для машин без udisks2: GUI
  запускает его через `pkexec` и читает прогресс по строчному протоколу
  (`PROGRESS`/`LOG`/`RESULT`). PolicyKit-файл в `data/`.

## Что перенесено из Rufus, и как

| Rufus (Win32)                          | rufox (Linux)                                |
|----------------------------------------|----------------------------------------------|
| SetupAPI, `IOCTL_STORAGE_*`            | sysfs + libblkid (`dev.c`)                   |
| `IsFilteredDrive()`                    | защита boot-диска, оптики, dm/md/zram (`dev.c`) |
| `CreatePartition()` (`drive.c`)        | планировщик раскладки + libparted (`partition.c`) |
| `IOCTL_DISK_SET_DRIVE_LAYOUT_EX`       | `ped_disk_new_fresh` + `ped_disk_commit`     |
| `WriteMBR()` / ms-sys                  | `diskio.c` + `/usr/lib/syslinux/mbr/*.bin`   |
| `FormatEx()` / VDS                     | `mkfs.*` (`format.c`)                        |
| `WriteDrive()` (DD)                    | `rfx_disk_write_image()` с ретраями 5×5 c    |
| `ImageScanThread` / `check_iso_props`  | `iso_scan.c` на libcdio/libiso9660/libudf    |
| `ExtractISO` / `fix_config()`          | `iso_extract.c` (патчи `persistent`, меток, `inst.repo`) |
| bled (gzip/xz/bz2/zstd/VHD)            | детект + честное сообщение; декомпрессия в tmp |
| `badblocks.c`                          | `badblocks.c` + `badblocks(8)` когда есть    |
| wimlib + bcdboot (Windows To Go)       | wimtools + копирование `bootmgfw.efi` в ESP  |
| LabConfig-обходы Win11                 | unattend.xml с `reg add …\LabConfig`         |
| `RUFUS_IMG_REPORT`                     | `RfxImgReport` (те же поля и имена)          |
| `CHECK_FOR_USER_CANCEL`                | `RfxCancel` + `RFX_CHECK_CANCEL`             |

Подробности и обоснования каждого решения — в [`docs/PORTING.md`](docs/PORTING.md).

## Проверено сквозняком

Полный цикл `write` прогнан в песочнице на файл-образе диска (loop-устройств
там нет, поэтому файловая цель адресуется по смещениям — тот же приём, что
Rufus применяет к VHD):

* защитный MBR `55AA` + тип `0xEE`, диск-сигнатура Rufus-стиля;
* GPT: заголовок и таблица с валидными CRC32, backup GPT на месте, главный
  раздел — **Microsoft Basic Data**, а не ESP (иначе Windows-инсталлятор
  ругается на «два ESP»);
* FAT32 от настоящего `mkfs.fat` (≥65525 кластеров), метка из ISO;
* дерево ISO скопировано, `isolinux.cfg` и `grub.cfg` пропатчены
  (`persistent` вставлен в командную строку ядра);
* ext3-раздел `casper-rw` с журналом для persistence;
* MBR+BIOS+«fixes for old BIOSes»: offset 64512 (удвоенный трек), boot=0x80,
  тип 0x0C, SYSLINUX `mbr.bin` байт-в-байт;
* DD-режим: побайтово идентичная копия + верификация, ~640 МБ/с.

## Лицензия

GPL-3.0-or-later. Производная работа от Rufus © 2011–2026 Pete Batard;
исходники апстрима разбирались по тэгу `v4.15`.
