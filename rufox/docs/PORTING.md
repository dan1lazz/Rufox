# Заметки о портировании: Rufus 4.15 → Linux

Этот документ фиксирует, *почему* каждое решение принято именно так, и где
Linux принципиально отличается от Windows. Читается вместе с `src/`.

## 0. Стратегия

Rufus — 46 тысяч строк Win32-C. Портовать его можно двумя способами:

1. **Эмуляция Win32** — написать прослойку под `HANDLE`, `CreateFileU`,
   `DeviceIoControl`. Получается вечный долг: каждый IOCTL придётся
   изображать вручную, а семантика блокировок Windows всё равно не совпадает.
2. **Перенос семантики** — для каждой подсистемы Rufus найти/linux-эквивалент
   и сохранить *порядок операций и числа*, которые делают носитель
   загрузочным. Именно числа (смещения, размеры ESP, маски FAT, порядок
   патчей конфигов) и есть ноу-хау Rufus.

Выбран путь 2. Следствие: структура модулей повторяет `src/*.c` апстрима
(`format.c → format.c`, `iso.c → iso_scan.c/iso_extract.c`, …), а имена
структур и enum-ов совпадают (`RfxImgReport` ≙ `RUFUS_IMG_REPORT`, поля
`has_efi`, `uses_casper`, `sl_version` и т.д.), чтобы логи и багрепорты
можно было сверять с апстримом один-в-один.

## 1. Перечисление устройств (`dev.c`)

Rufus: SetupAPI (`GUID_DEVINTERFACE_DISK`) + `IOCTL_STORAGE_QUERY_PROPERTY`
+ `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` + `hdd_vs_ufd.h`.

Linux: `/sys/block/*` — единственный источник, который не требует прав:
`removable`, `ro`, `queue/rotational`, `queue/{logical,physical}_block_size`,
`device/{vendor,model,serial}`. Геометрия (`BLKSSZGET`, `BLKPBSZGET`,
`BLKGETSIZE64`, `HDIO_GETGEO`) — для размеров и «трека», которым Rufus
выравнивает дополнительные разделы.

Фильтрация (`rfx_dev_is_filtered`) повторяет `IsFilteredDrive()`:
* диск, с которого загружена текущая система — **никогда** не предлагаем.
  Определяется по `/proc/mounts` для `/`, `/boot`, `/home`, `/var`, `/usr`
  плюс рекурсивный обход `/sys/block/*/slaves` для device-mapper/LVM/MD
  (root на `/dev/dm-0` помечает `/dev/sda`); плюс `root=` из `/proc/cmdline`.
* оптика (`sr*`), флоппи (`fd*`), `dm-*`, `md*`, `ram*`, `zram*`, read-only.
* loop разрешён — это наш тестовый путь.

Hotplug: GUdev (`uevent` add/remove/change) когда собран с `gudev`, иначе
опрос раз в 2 с — аналог `WM_DEVICECHANGE` + `RefreshTimer`.

## 2. Раскладка разделов (`partition.c`)

Ядро — `rfx_partition_plan()`, чистая функция без I/O, побуквенно повторяющая
`CreatePartition()`:

* первый раздел: `1 MiB`, либо при «Fixes for old BIOSes» —
  `CEILING_ALIGN(bytes_per_track, cluster) * 2` (удвоение трека появилось в
  Rufus, чтобы GRUB2 влез в embedding area; проверено: 63·512·2 = 64512);
* ESP 260 MiB в начало — только для GPT+Windows To Go (как в апстриме);
* MSR 128 MiB следом;
* дополнительные разделы (persistence / UEFI:NTFS / BIOS-compat) кладутся
  **с конца** диска обходом назад с floor-выравниванием по треку;
* главный раздел = остаток, floor по треку и по кластеру;
* для GPT резервируются 33 сектора в хвосте (backup GPT).

Запись таблицы — libparted (`ped_disk_new_fresh`/`ped_disk_add_partition`/
`ped_disk_commit`): он один корректно сериализует protective MBR, backup GPT
и CRC. Флаги (`PED_PARTITION_ESP`, `PED_PARTITION_MSFT_RESERVED`,
`PED_PARTITION_HIDDEN`) дают те же type GUID / MBR-байты, что Rufus выставляет
вручную в `DRIVE_LAYOUT_INFORMATION_EX`.

**Важный баг, пойманный тестом:** у Rufus главный раздел — `PARTITION_MICROSOFT_DATA`,
а не ESP; две ESP на диске ломают установщик Windows на «Copying Windows Files».
Первая версия порта помечала главный раздел как ESP (type `C12A7328-…`);
юнит-тест `test_partition_plan` это теперь ловит.

После коммита Rufus затирает головы разделов (`ClearPartition`, 128 секторов),
чтобы старые суперблоки не «воскресли» — делаем то же.

MBR-фиксации после форматирования (`rfx_disk_fixup_mbr`) — порт `WriteMBR()`:
FAT16 → 0x0E, FAT32 → 0x0C (FormatEx/mkfs оставляют CHS-варианты), boot
indicator 0x80, а для WinPE-образа без minint — 0x81 («masquerade as second
disk»). Для GPT пишем свою диск-сигнатуру, как Rufus помечает схему.

## 3. Форматирование (`format.c`)

`FormatEx()`/VDS → канонические `mkfs.*`: vfat (dosfstools), ntfs (ntfs-3g),
exfat (exfatprogs), ext2/3/4 (e2fsprogs), udf (udftools). Таблицы допустимых
ФС и размеров кластера — из `SetAllowedFileSystems()`/`SetClusterSizes()`:
в т.ч. правило «FAT32 требует ≥65525 кластеров, иначе молча станет FAT16»
(первая версия порта сравнивала в обратную сторону — тест
`test_cluster_gate`).

Метки — `ToValidLabel()`: FAT ≤11 символов, верхний регистр, без `./\:;+,*?=[]"`.

### Файловая цель (образ диска вместо /dev/sdX)

Нужна для CI/песочниц без loop. Ключевая находка: **`mformat -i file@@offset`
в mtools 4.0.32 затирает первые ~8 КиБ всего файла** — protective MBR, GPT-
заголовок и таблицу. Проверено маркерным тестом. Поэтому форматирование
файловой цели идёт так: mkfs на разреженном временном файле размером с раздел
→ `sendfile()` в образ по смещению раздела (у mkfs.vfat/ntfs/exfat нет
опции offset вообще; у ext есть `-E offset`, но единый путь проще). Копия
побайтово совпадает с тем, что mkfs написал бы на реальный раздел: boot-сектор
FAT хранит *относительные* номера секторов.

Копирование дерева в несмонтированный FAT — mtools (`mmd` по одному уровню,
`mcopy -s -n` по элементам верхнего уровня; `mcopy -s DIR/.` mtools
отказывает: «Cannot create entry named . or ..»).

## 4. Анализ образов (`iso_scan.c`)

Rufus тащит свой форк libcdio; мы берём системные libcdio/libiso9660/libudf
(та же родословная). Обход = `check_iso_props()`: syslinux-конфиги, каталоги
GRUB2, `/casper`, `/proxmox`, маркеры Manjaro, `bootmgr[.efi]`, `grldr`,
`md5sum*`, ReactOS, `efi*.img`, загрузчики под `/efi/**` (а не только
`/efi/boot/` — привет Fedora), обрывок-симлинк `bootx64.efi` <256 байт у Mint
(фолбэк на El-Torito), `install.wim|esd|swm…` в `/sources/`, PE-маркеры,
файлы ≥4 ГиБ, глубокие каталоги, длинные имена. Версия ISOLINUX вычитывается
из баннера бинаря (`ISOLINUX 6.04` в фикстуре — проверено). Версия Windows —
из заголовка WIM (`MSWIM`, версия в +12: 0xCE00/0xD800/0xE000/0xE100).

DD-детект: PVD ISO9660 на 32768 / AVDP UDF на 256·2048; isohybrid — валидный
MBR с boot-флагом и LBA 0/1/64. Сжатие (bled): суффикс + магические байты
(gzip/bz2/xz/lzma/zstd/conectix-VHD); сжатый образ помечается
`disable_iso → DD` и честно говорит пользователю про декомпрессию.

## 5. Распаковка и патчи конфигов (`iso_extract.c`)

`fix_config()` — причина, по которой распакованное дерево вообще грузится:
* persistence: пять диалектов в порядке Rufus — `file=/cdrom/preseed` →
  `persistent file=…`, `boot=casper` → `boot=casper persistent`,
  `/casper/vmlinuz` (linux/kernel), `boot=live` → `boot=live persistence`;
  плюс удаление `maybe-ubiquity` для GRUB;
* метка тома: ISO-метка → метка флешки, пробелы как `\x20`, токены
  `options/append/linux/linuxefi/$linux/search/for` (openSUSE с её
  `set linux=linux` и Arch с `search --label` покрыты);
* RHEL8+: `inst.stage2` → `inst.repo`, кроме netinst-образов;
* FreeNAS: `cd9660:/dev/iso9660/…` → `msdosfs:/dev/msdosfs/…`.

Данные читаются `pread()` по `lsn*2048` (экстенты ISO9660 непрерывны);
фолбэк — `iso9660_iso_seek_read()` для экзотики. Имена санитизируются как в
Rufus (полноширинные замены для `\/:*?"<>|`, обрезка точек/пробелов в хвосте).
`ldlinux.sys` из образа пропускается, чтобы не затереть наш.

## 6. Загрузчики (`bootloaders.c`)

Rufus возит бинари ресурсами внутри .exe. На Linux они уже установлены
дистрибутивом, и мы их находим (`rfx_boot_find_resource`):
`/usr/lib/syslinux/mbr/{mbr,gptmbr,altmbr}.bin`,
`/usr/lib/ISOLINUX/isohdpfx.bin`, `/usr/lib/grub/{i386-pc,x86_64-efi}`.
MBR пишется как в Rufus: 440 байт кода + сигнатура диска + номер диска в
0x1BD + сохранённая таблица разделов + `55AA` (сверено побайтово с
`mbr.bin`). Полная установка SYSLINUX (ldlinux.sys с секторной картой)
делегирована системному `syslinux`/`extlinux`: рукописная перезапись секторной
карты FAT — тот случай, где ошибка даёт необъяснимо негрузящуюся флешку, и
апстрим делает то же самое своим кодом ms-sys. GRUB2 — `grub-install`
(`--removable --no-nvram` для UEFI). UEFI:NTFS-шим ищется как
`uefi-ntfs.img` в datadir (в апстриме — встроенный ресурс).

## 7. Windows-специфика (`wintogo.c`, твики в `job.c`)

Windows To Go = wimlib (`wiminfo` для списка редакций, `wimapply` для
распаковки) + `bcdboot`. bcdboot — Windows-бинарь, поэтому:
* UEFI-цель делаем полностью сами: `bootmgfw.efi` из применённого образа →
  `\EFI\Boot\bootx64.efi` на ESP (ровно то, что делает `bcdboot /s` для
  removable-носителя);
* BIOS-цель честно сообщает, что BCD-улей умеет собрать только bcdboot,
  и печатает команду для ручного добивания из Windows.

Обходы требований Win11 = `HKLM\SYSTEM\Setup\LabConfig` с
`Bypass{TPM,SecureBoot,RAM}Check=1` (значения взяты из `wue.c`); улей живёт
внутри WIM, поэтому пишем unattend.xml с `reg add` в pass windowsPE —
эквивалент того, что WUE делает живьём.

## 8. Оркестрация (`job.c`)

Порядок шагов = `FormatThread()`: probe → scan → эксклюзивный `flock` →
unmount всего → badblocks → (zero) → ClearMBRGPT → план+таблица → mkfs →
MBR-фиксации → mount → extract/patch → bootloader → Win-твики → persistence →
extended label → sync/umount/BLKRRPART. DD-путь: анализ старого MBR →
запись с 5 ретраями по 5 с (как `WRITE_RETRIES`/`WRITE_TIMEOUT`) → опционная
верификация. Fast-zeroing с пропуском уже-пустых блоков и backoff-счётчиком 15
— тоже из апстрима.

Прогресс — `RfxProgressCb` с весами операций; GUI маршалит его в главный
поток через `GAsyncQueue` + idle-source (аналог `PostMessage`).

## 9. Привилегии

Rufus — один приподнятый процесс. Linux-идиома: udisks2 с polkit-запросами
или pkexec. Реализован второй вариант: `rufox-helper` (libexec) принимает те
же флаги (общий парсер `src/cliopts.c`, аргументы сериализуются и
десериализуются одной функцией — рассогласование невозможно) и гонит job,
отдавая GUI строки `PROGRESS/LOG/RESULT`. На машинах с udisks2 и root-запуске
helper не нужен вовсе.

## 10. Известные ограничения

* Сжатые образы (bled): детект есть, потоковая декомпрессия на устройство —
  нет; предлагается предварительно распаковать. В апстриме этим занимается
  встроенный bled.
* ReFS: только в списке ФС-комбо, форматирования нет (как и везде вне Windows).
* Windows To Go на BIOS-цель требует bcdboot (см. §7).
* UDF-форматирование требует udftools; запись на UDF-цель не тестировалась.
* `syslinux` как исполняемый файл есть не во всех дистрибутивах (Debian
  кладёт MBR/модули, но не инсталлятор) — тогда ставится только MBR, о чём
  печатается явное предупреждение.
