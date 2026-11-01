# Ревью порта Rufus → Linux (`rufox` 4.15.0)

**Дата проверки:** 2026-10-25
**Источник:** `rufox.zip` с MediaFire, sha256 `2361937ac928085fac6fbb54d1dfcd433d4cbe1cd8acf422a8ebd5b4139f9750`, 161 847 байт, 80 записей
**Объём проекта:** 37 файлов, 8 528 строк (C + meson + shell)
**Стенд:** Debian 12, gcc 12.2.0, meson 1.0.1, GTK 4.8.3, libadwaita 1.2.2, glib 2.74.6, gudev 237, blkid 2.38.1, libparted 3.5, libcdio/libiso9660/libudf 2.1.0, udisks2 2.9.4

---

## TL;DR

Движок — **настоящий, а не обёртка над `dd`**. Логика раскладки, MBR-хирургии, анализа образов и патчинга конфигов перенесена добросовестно и выдержала мою независимую побайтовую проверку: 30/30 моих тестов разметки, 48/48 ваших юнит-тестов, DD побайтово идентичен (sha256), `mbr.bin` совпал байт-в-байт, offset 64512 подтверждён, тип раздела — Microsoft Basic Data, а не ESP. Ноль предупреждений компилятора на 8.5k строк при `-Wall -Wextra`.

Но есть три вещи, которые надо исправить до всего остального:

1. **Архив неполный — проект в поставленном виде не собирается.** Отсутствуют `src/wintogo.c` и весь каталог `src/ui/` (4 файла). `meson setup` падает на строке 67. GUI проверить нечем физически.
2. **Найдено 5 дефектов уровня HIGH в ядре записи**, два из них я воспроизвёл экспериментально: `clear_mbr_gpt` чистит хвост на 16 секторов вместо 33 (остатки backup GPT выживают), а DD-путь не чистит хвост вообще. Плюс `flock` при неудаче только предупреждает и продолжает стирать диск, `fsync` с `EIO` считается успехом, а верификация читает из page cache и потому не проверяет ничего.
3. **Фингерпринты MBR перепутаны** — на реальных бинарниках syslinux определитель выдаёт неверный результат в 4 случаях из 4.

Общая оценка: архитектура и перенос семантики — 8.5/10; надёжность пути записи — 5/10; собираемость поставки — 0/10; UI — не оцениваемо (отсутствует).

---

## 1. BLOCKER: архив неполный

### 1.1 Отсутствующие файлы

| Файл | Где требуется | Последствие |
|---|---|---|
| `src/wintogo.c` | `meson.build:89` (`core_src`) | meson: `Nonexistent file` |
| `src/ui/rufox-gui.h` | `src/main_gui.c:11` | нечем собрать GUI |
| `src/ui/engine.c` | `meson.build:142` | нечем собрать GUI |
| `src/ui/app.c` | `meson.build:142` | нечем собрать GUI |
| `src/ui/window.c` | `meson.build:142` | нечем собрать GUI |
| `src/ui/` (каталог) | `meson.build:67` `include_directories` | **configure падает немедленно** |
| `data/rufox-gui.desktop` | `meson.build:148` `install_data` | `ninja install` упадёт |

Функции, которые должен предоставлять `src/wintogo.c` (объявлены в `src/rufox.h:661-663`, используются в `src/main_cli.c:187`):

```c
bool        rfx_wintogo_supported(const RfxImgReport *img);
GPtrArray*  rfx_wintogo_list_editions(const char *wim_path);
bool        rfx_wintogo_create(const char *devpart, const RfxImgReport *img,
                               const RfxOptions *opt, RfxProgressCb cb,
                               gpointer ud, RfxCancel *cancel);
```

Фактическая ошибка конфигурации:

```
meson.build:67:0: ERROR: Include dir src/ui does not exist.
```

### 1.2 Артефакты упаковки

* `src/cancel(1).c` — побайтовый дубль `src/cancel.c` (проверено `diff`), в сборке не участвует;
* `data/rufox-gui.desktop.download` и `data/rufox-gui.desktop(1).download` — **незавершённые загрузки Chrome** (расширение `.download`, суффикс `(1)`). Содержимое обоих файлов валидное и идентичное — достаточно переименовать один в `rufox-gui.desktop`;
* `.DS_Store`, `__MACOSX/` — мусор от упаковки на macOS.

Картина складывается такая: zip собирался из папки загрузок, и часть файлов в него не попала. **Нужно перезалить архив**, собрав его из рабочего дерева (`git archive` или `zip -r` из корня репозитория), а не из `~/Downloads`.

---

## 2. Сборка

### 2.1 Как поставлено — не собирается

```
$ meson setup build
...
Run-time dependency gtk4 found: YES 4.8.3
Run-time dependency libadwaita-1 found: YES 1.2.2
Run-time dependency gudev-1.0 found: YES 237
Run-time dependency udisks2 found: YES 2.9.4
Configuring rufox-config.h using configuration
meson.build:67:0: ERROR: Include dir src/ui does not exist.
```

### 2.2 С диагностическими заглушками — собирается идеально

Чтобы оценить остальной код, я в отдельной копии (`buildcheck/`, оригинал не тронут):
создал пустой `src/ui/`, убрал `src/wintogo.c` из `core_src`, добавил заглушку `wintogo_stub.c` с тремя функциями выше, собрал с `-Dgui=disabled`.

Результат:

```
[19/24] Linking static target librufox-core.a
[23/24] Linking target rufox-helper
[ 5/ 5] Linking target rufox-tests
[ 5/ 5] Linking target rufox
EXIT: 0
```

* **17 из 18 файлов ядра компилируются чисто.**
* **0 предупреждений** при `warning_level=2` (`-Wall -Wextra`) — на 8 528 строк C. Для проекта такого рода это редкость и сильный признак дисциплины.
* Единственная ошибка линковки без заглушки — `undefined reference to 'rfx_wintogo_supported'` из `main_cli.c:187`. То есть всё остальное самодостаточно.
* Зависимости в README указаны верно. Отдельно отмечу аккуратность `meson.build`: `parted` корректно ищется через `cc.find_library` (pkg-config-имя — `libparted`), для `libiso9660`/`libudf` есть двойной фолбэк (pkg-config → `find_library`), GUI/hotplug/udisks вынесены в feature-опции.

### 2.3 Тесты

```
$ ./build/rufox-tests
SKIP iso scan: no fixture at /tmp/rufox-fixture.iso      # без фикстуры
40 checks, 0 failure(s)

$ sh tests/make_fixture.sh /tmp/rufox-fixture.iso          # + xorriso, isolinux
$ ./build/rufox-tests
48 checks, 0 failure(s)
```

**48 проверок, 0 провалов — ровно то число, которое заявлено в README.** Документация не приукрашивает. Фикстура собирается штатно, `ISOLINUX 6.04` в баннере совпадает с тем, что обещает `PORTING.md §4`.

---

## 3. Что подтверждено независимо (сильные стороны)

Я не полагался на ваши тесты и написал собственный побайтовый аудит результата (`verify_image.py`, `tail_probe.py`). Полный цикл `write` прогонялся на файл-образах.

### 3.1 GPT + FAT32 + UEFI, образ 51 МБ на диск 4 ГиБ — **30/30 проверок**

| Проверка | Результат |
|---|---|
| Protective MBR: сигнатура `55AA` | ✅ |
| Ровно одна запись типа `0xEE`, LBA 1, размер = весь диск | ✅ `size=8388607` |
| Диск-сигнатура в стиле Rufus @0x1B8 | ✅ `0x53554652` = «RFUS» |
| GPT-заголовок `EFI PART`, rev 0x10000, hdr_size 92 | ✅ |
| **CRC32 заголовка GPT валиден** | ✅ `0xDC5AE23F` |
| **CRC32 таблицы разделов валиден** | ✅ `0x30D9CDE8` |
| MyLBA=1, AltLBA=последний сектор, FirstUsable=34 | ✅ |
| Backup GPT: сигнатура, MyLBA/AltLBA переставлены, таблица перед заголовком | ✅ |
| Backup-таблица побайтово равна основной | ✅ |
| **Главный раздел — Microsoft Basic Data, НЕ ESP** | ✅ `EBD0A0A2-…` |
| Старт ровно в 1 МиБ (LBA 2048), выравнивание 1 МиБ | ✅ |
| Хвост под backup GPT ≥ 33 секторов | ✅ 63 сектора |
| FAT32 от настоящего `mkfs.fat`: 512 б/сектор, 64 сектора/кластер (32 КиБ) | ✅ |
| **Кластеров ≥ 65525** (иначе это был бы FAT16) | ✅ 131 006 |
| Метка тома `UBUNTU2404L` — из ISO-метки, по правилам FAT | ✅ |
| Нет остатков ISO PVD (`CD001`) на 32768 | ✅ |

Пункт про Microsoft Basic Data — тот самый баг, который `PORTING.md §2` описывает как пойманный тестом. **Подтверждаю независимо: он действительно исправлен.** Это не мелочь — две ESP ломают установщик Windows на «Copying Windows Files».

### 3.2 MBR + BIOS + `--old-bios-fixes`

| Проверка | Результат |
|---|---|
| Основной раздел на offset **64512** (LBA 126 = 2×63×512) | ✅ ровно как в Rufus |
| Boot indicator `0x80` | ✅ |
| Тип раздела **`0x0C`** (FAT32-LBA), а не `0x0B`, который оставляет `mkfs` | ✅ fixup отработал |
| Drive number @0x1BD = `0x80` | ✅ |
| Первые 440 байт **байт-в-байт** равны `/usr/lib/syslinux/mbr/mbr.bin` | ✅ `identical 440 bytes: True` |
| Таблица разделов и `55AA` сохранены | ✅ |
| BIOS-compat раздел: тип `0x83`, 63 сектора в конце | ✅ |

### 3.3 DD-режим

```
src size: 53477376   dst size: 134217728
first 53477376 bytes identical: True
tail beyond image is zero: True
sha256 src       : bae41f92feed7c336a1faf5cba0179a7
sha256 dst[:n]   : bae41f92feed7c336a1faf5cba0179a7
```

Побайтовая идентичность, хвост не тронут, «Verification successful».

### 3.4 Persistence и патчинг конфигов

`-p 512M` → «Patched 2 config file(s)», создан ext3-раздел `casper-rw`. Оба диалекта отработали **точно** по `PORTING.md §5`:

```diff
# isolinux.cfg — диалект "file=/cdrom/preseed → persistent file=…"
- append boot=casper initrd=/casper/initrd.lz file=/cdrom/preseed/ubuntu.seed quiet splash
+ append boot=casper initrd=/casper/initrd.lz persistent file=/cdrom/preseed/ubuntu.seed quiet splash

# grub.cfg — диалект "boot=casper → boot=casper persistent"
- linux /casper/vmlinuz boot=casper initrd=/casper/initrd.lz quiet splash ---
+ linux /casper/vmlinuz boot=casper persistent initrd=/casper/initrd.lz quiet splash ---
```

Суперблок ext3: `magic=0xEF53`, `label='casper-rw'`, `has_journal=True`. Без `-p` конфиги не трогаются — поведение корректное.

### 3.5 Анализ образа

`rufox info -i fixture.iso` на синтетической фикстуре дал полный и правильный отчёт: ISOLINUX 6.04 (версия вычитана из баннера бинаря), GRUB2, casper/live, isohybrid, UEFI-загрузчик `/efi/boot/bootx64.efi` с маской `0x2`, предложенная метка `UBUNTU2404L`, список допустимых ФС, метка persistence `casper-rw`, projected size.

### 3.6 Безопасность: то, что сделано правильно

Проверил отдельно, потому что для такого инструмента это главное:

* **Защита загрузочного диска enforced внутри движка, а не в UI.** `job.c:397` вызывает `rfx_dev_is_filtered()` до открытия устройства на запись. Это значит, что привилегированный `rufox-helper` (root через pkexec) соблюдает её независимо от того, что ему передали в `-d`. Правильное архитектурное решение — ровно так и должно быть.
* **Shell-инъекций нет.** `grep` по `system(`, `popen(`, `g_spawn_command_line`, `execlp`, `/bin/sh` по всему `src/` и `tools/` — пусто. Всё идёт через `g_subprocess_launcher_spawnv()` с argv-массивом.
* **Инъекция аргументов через метку невозможна.** `label_args()` (`format.c:261-284`) кладёт метку отдельным элементом argv сразу после `-n`/`-L`; getopt потребляет следующий элемент как значение опции даже если он начинается с `-`. Плюс метка проходит `rfx_format_label_valid()` с фолбэком `RUFOX`.
* **Directory traversal при распаковке ISO исключён.** `sanitize_filename()` (`iso_extract.c:42-72`) заменяет `\ / : * ? " < > |` на полноширинные аналоги и срезает хвостовые точки/пробелы; `".."` → `""` → `"_"`.
* **`rproc.c:31`** — `unsetenv("LD_PRELOAD")` для дочерних процессов: правильная паранойя.
* **Polkit-политика** (`data/rufox-helper.policy`) — `auth_admin` / `auth_admin_keep`, а не `yes`. То есть root-половина не доступна без аутентификации администратора. Корректно.
* **Graceful degradation**: в песочнице нет loop-устройств → `losetup` падает → код честно предупреждает и переключается на прямой доступ через mtools, job завершается успешно. Это хорошо спроектированный фолбэк.

### 3.7 Найденная в `format.c` деталь, заслуживающая отдельной похвалы

Комментарий в `format.c:290-296` документирует, что `mformat -i file@@offset` в mtools 4.0.32 **затирает первые 8 КиБ всего файла**, убивая protective MBR и GPT. Это нетривиальная находка, и выбранный обход (mkfs на разреженном scratch-файле → `sendfile()` по смещению раздела) корректен: boot-сектор FAT хранит относительные номера секторов, поэтому результат побайтово совпадает с mkfs на реальном разделе — что мои проверки в §3.1 и подтвердили.

---

## 4. Дефекты

### HIGH

---

#### H1. `clear_mbr_gpt` чистит хвост на 16 секторов вместо 33 — остатки backup GPT выживают

`src/diskio.c:200`

```c
size_t tail_len = buf_len / 8;          /* buf_len = 512*128 = 64 KiB → tail = 8 KiB = 16 секторов */
```

В голову диска пишется `MAX_SECTORS_TO_CLEAR = 128` секторов (как в Rufus), а в хвост — в 8 раз меньше, без всякого обоснования. Backup GPT занимает **33 сектора**: 1 заголовок (последний LBA) + 32 сектора таблицы (LBA `ns-33 … ns-2`). Комментарий прямо над строкой гласит «The backup GPT lives at the very end» — но 8 КиБ его не покрывают.

**Доказательство** (харнесс `harness_clear.c`, слинкован с вашей же `librufox-core.a`; имитирует stale backup GPT и вызывает функцию напрямую):

```
До clear_mbr_gpt:
  LBA 1      (primary GPT hdr) : nonzero=13
  LBA ns-33  (backup table)    : nonzero=128
  LBA ns-1   (backup GPT hdr)  : nonzero=13

rfx_disk_clear_mbr_gpt() вернула true

После clear_mbr_gpt:
  LBA 1      (primary GPT hdr) : nonzero=0    стёрто
  LBA ns-33  (backup table)    : nonzero=128  <-- ВЫЖИЛО
  LBA ns-1   (backup GPT hdr)  : nonzero=0    стёрто
```

Функция возвращает `true`, рапортуя успех, при том что запись backup-таблицы цела.

**Почему это не осталось чисто теоретическим** — см. H2. В большинстве ISO-сценариев следы случайно затираются основным разделом (он дотягивается до конца диска) и шагом ClearPartition; я это специально проверил, чтобы не заявить баг на пустом месте — при `GPT → MBR` хвост действительно оказался чист. Но полагаться на случайное перекрытие в утилите для записи дисков нельзя.

**Патч:**

```c
-	size_t tail_len = buf_len / 8;
+	/* Backup GPT = 1 header + 32 table sectors = 33 sectors. Rufus clears
+	 * MAX_SECTORS_TO_CLEAR at both ends; there is no reason to clear less
+	 * at the tail, and clearing less leaves stale partition entries. */
+	size_t tail_len = buf_len;
```

---

#### H2. DD-путь не чистит старую разметку вообще — фантомный GPT после записи

`src/job.c:521-560`

DD-ветка вызывает только `rfx_disk_analyze_mbr()` (информационно) и `rfx_disk_write_image()`. `rfx_disk_clear_mbr_gpt()` в проекте вызывается **ровно в одном месте** — `job.c:577`, в ISO-ветке:

```
$ grep -n 'clear_mbr_gpt' src/*.c
src/diskio.c:182:bool rfx_disk_clear_mbr_gpt(int fd, uint64_t disk_size, uint32_t sector_size)
src/job.c:577:        if (!rfx_disk_clear_mbr_gpt(dev_fd, j->Drive.DiskSize, j->Drive.SectorSize))
```

Если образ меньше устройства — а это типичный случай (Raspberry Pi OS, гибридные ISO, прошивки, любые `.img`), — хвост диска остаётся нетронутым.

**Доказательство** (сквозной тест, 1 GiB файл-образ):

```
шаг1: GPT-раскладка записана
   грязные LBA в последних 64 секторах: [2097119, 2097151]      # ns-33 и ns-1

шаг2: DD-запись 51-мегабайтного образа на этот же 1 GiB диск
   ✔ Done in 0.0 s     exit=0

хвост ПОСЛЕ DD:
   LBA 2097151 (ns-1): ненулевых 47/512  first16=4546492050415254000001005c000000
                                          ^^^^^^^^ "EFI PART"  <== BACKUP GPT HEADER
   грязные LBA: [2097119, 2097151]                               # НИЧЕГО НЕ ИЗМЕНИЛОСЬ
```

Диск после такой записи несёт новый MBR из образа в LBA 0 **и** старый backup GPT в конце. Реальные последствия:

* ядро Linux логирует `GPT: primary header thinks Alt. header is not at the end of the disk`;
* `parted` предлагает «fix» при каждом открытии устройства;
* `blkid`/`lsblk`/GNOME Disks могут показать фантомный тип таблицы;
* Windows Disk Management на таких дисках ведёт себя непредсказуемо.

Это ровно тот класс дефектов, от которого Rufus защищает вызовом `ClearMBRGPT()` — и функция у вас уже написана, она просто не вызывается на этом пути.

**Патч** (`job.c`, DD-ветка, перед `rfx_disk_write_image`):

```c
 			/* Rufus analyzes the existing MBR before overwriting it. */
 			rfx_disk_analyze_mbr(dev_fd, "Drive");
 
+			/* A DD image smaller than the device leaves the previous
+			 * backup GPT at the end of the disk, which makes the kernel,
+			 * parted and Windows see a phantom GPT. Clear both ends first. */
+			if (!rfx_disk_clear_mbr_gpt(dev_fd, j->Drive.DiskSize,
+			                            j->Drive.SectorSize))
+				uprintfw("Could not clear the old partition records: %s",
+				         rfx_last_error_message());
+
 			if (!rfx_disk_write_image(src_fd, j->Image.image_size, dev_fd,
```

Применять вместе с H1, иначе будет вычищено 16 секторов из 33.

---

#### H3. Провал `flock()` не останавливает запись

`src/job.c:463-468`

```c
/* Rufus locks the physical drive so that Windows cannot interfere.
 * The Linux equivalent is flock(LOCK_EX|LOCK_NB). */
if (is_real_device(j->DevicePath) &&
    flock(dev_fd, LOCK_EX | LOCK_NB) != 0)
        uprintfw("Could not get an exclusive lock on %s: %s",
                 j->DevicePath, strerror(errno));
```

Блокировка не получена → **warning в лог и продолжение стирания диска**. Комментарий сам объясняет назначение блокировки («чтобы никто не вмешивался»), но при отказе код делает прямо противоположное.

Сценарий: пользователь запустил запись, GUI подвис, он запустил вторую копию (или рядом работает `dd`, `gnome-disks`, другая утилита записи). Второй экземпляр получает отказ `flock`, пишет warning, который в GUI тонет в логе, и начинает писать таблицу разделов и ФС поверх первого. Результат — гарантированно испорченный носитель и, что хуже, **сообщение об успехе**.

Rufus трактует отказ эксклюзивного доступа как фатальный и прекращает операцию. Здесь должно быть то же.

**Патч:**

```c
-		if (is_real_device(j->DevicePath) &&
-		    flock(dev_fd, LOCK_EX | LOCK_NB) != 0)
-			uprintfw("Could not get an exclusive lock on %s: %s",
-			         j->DevicePath, strerror(errno));
+		if (is_real_device(j->DevicePath) &&
+		    flock(dev_fd, LOCK_EX | LOCK_NB) != 0) {
+			int e = errno;   /* FAIL() сам читает errno — сохраним его */
+			FAIL(ERR_DEVICE_BUSY,
+			     "'%s' is being used by another process (could not obtain "
+			     "an exclusive lock): %s", j->DevicePath, strerror(e));
+		}
```

---

#### H4. `fsync()` с `EIO` считается успехом — «✔ Done» на умирающей флешке

`src/rutil.c:239-250`

```c
bool rfx_sync_block(int fd)
{
	if (fd < 0) return false;
	/* Flush the page cache *and* the device's own volatile write cache. */
	syncfs(fd);
	if (fsync(fd) != 0) {
		if (errno == EINVAL || errno == EIO)
			return true;	/* not fatal on some kernels */
		return false;
	}
	return true;
}
```

`EINVAL` — законный случай (объект не поддерживает fsync). **`EIO` — нет**: это прямое сообщение ядра «данные не дошли до носителя». Для утилиты записи это наихудший возможный исход — носитель деградирует, запись физически не удалась, а пользователь получает зелёную галочку и идёт устанавливать систему с флешки, которая не boot-able.

Особенно опасно в связке с H5: верификация тоже ничего не поймает, потому что читает из кэша.

Дополнительно: `syncfs(fd)` на fd блочного устройства, открытого сырым, — вызов бессмысленный (суперблока файловой системы у этого fd нет). Реальный flush даёт `fsync()`; для сброса буферного кэша ядра нужен `BLKFLSBUF`.

**Патч:**

```c
-		if (errno == EINVAL || errno == EIO)
-			return true;	/* not fatal on some kernels */
+		if (errno == EINVAL)
+			return true;	/* fd не поддерживает fsync — не наша вина */
+		/* EIO here means the data never reached the media. Reporting
+		 * success would hand the user a corrupt, non-bootable stick. */
 		return false;
```

---

#### H5. Верификация читает из page cache и потому не проверяет носитель

`src/diskio.c:400-458`

```c
ssize_t ra = pread(src_fd, a, chunk, (off_t)done);
ssize_t rb = pread(dst_fd, b, chunk, (off_t)done);
...
if (memcmp(a, b, (size_t)ra) != 0) { ... }
```

`dst_fd` — тот же дескриптор, через который только что шла запись `pwrite()`. На Linux page cache блочного устройства когерентен с записью через тот же fd, поэтому `pread()` вернёт **то, что мы сами положили в кэш**, а не то, что лежит на флешке. Ни `BLKFLSBUF`, ни `O_DIRECT` в верификационном пути нет — и, как показал grep, `BLKFLSBUF` отсутствует во всём проекте:

```
$ grep -rn 'BLKFLSBUF' src/ tools/
(пусто)
```

Итог: `-V/--verify` всегда печатает «Verification successful», если только запись не упала раньше. Единственная цель верификации — поймать деградировавший носитель и тихую потерю данных — не достигается. Пользователь платит временем за проверку, которая ничего не проверяет.

**Патч** (в начало `rfx_disk_verify_image`, заголовки `<sys/ioctl.h>` и `<linux/fs.h>` в `diskio.c` уже подключены):

```c
 	uprintf("Verifying:");
+
+	/* Without this, pread() on dst_fd returns what we just wrote into the
+	 * page cache rather than what actually reached the media — the whole
+	 * point of verification. */
+#ifdef BLKFLSBUF
+	if (ioctl(dst_fd, BLKFLSBUF, 0) != 0 && errno != EINVAL && errno != ENOTTY)
+		uprintfw("Could not flush the device buffer cache: %s", strerror(errno));
+#endif
+
 	GTimer *t = g_timer_new();
```

Стоит также рассмотреть повторное открытие `dst_fd` с `O_DIRECT` для верификационного прохода — это сильнее, чем `BLKFLSBUF`, и не зависит от поведения конкретного ядра.

---

### MEDIUM

---

#### M1. Мёртвый guard в `probe_geometry` → на 4Kn «old BIOS fixes» даёт 516096 вместо 64512

`src/dev.c:317-318`

```c
if ((d->SectorsPerTrack * d->SectorSize) % d->SectorSize != 0)
	d->SectorsPerTrack = 63;
```

`(x * S) % S == 0` тождественно при любом целом `x` — условие **никогда** не истинно, ветка недостижима:

```
SectorsPerTrack=63  SectorSize=512  -> (spt*ss)%ss = 0   (сработает: False)
SectorsPerTrack=63  SectorSize=4096 -> (spt*ss)%ss = 0   (сработает: False)
SectorsPerTrack=255 SectorSize=4096 -> (spt*ss)%ss = 0   (сработает: False)
```

Комментарий над строкой заявляет намерение «make sure the track size is a multiple of the sector size … exactly like Rufus does before computing bytes_per_track», но код этого не делает.

Последствие через `partition.c:70-71`:

```c
const uint64_t bytes_per_track =
	(uint64_t)(drive->SectorsPerTrack ? drive->SectorsPerTrack : 63) * sector_size;
```

`HDIO_GETGEO` отдаёт `sectors` в 512-байтовых единицах. На 4Kn/512e-носителе (`SectorSize = 4096`) получаем:

```
rufox: 63 * 4096 = 258048  ->  old-BIOS offset = 2*track = 516096
Rufus: 63 * 512  =  32256  ->  old-BIOS offset = 2*track =  64512
```

То есть на 512-байтовых носителях заветные 64512 воспроизводятся точно (я это подтвердил в §3.2), а на 4Kn молча уезжают в 8 раз. `PORTING.md §2` приводит 64512 как инвариант — на 4Kn он не выполняется.

Комбинация «4Kn-флешка + Fixes for old BIOSes» редкая, поэтому MEDIUM, а не HIGH. Но это именно тот числовой инвариант, ради которого затевался перенос семантики.

**Рекомендация:** хранить «трек» в 512-байтовых единицах и выравнивать результат вверх до `sector_size`:

```c
-	const uint64_t bytes_per_track =
-		(uint64_t)(drive->SectorsPerTrack ? drive->SectorsPerTrack : 63) * sector_size;
+	/* HDIO_GETGEO reports sectors-per-track in 512-byte units. Rufus' CHS
+	 * track is 63*512 regardless of the media's sector size; on 4Kn we
+	 * still have to land on a sector boundary. */
+	const uint64_t bytes_per_track = RFX_CEILING_ALIGN(
+		(uint64_t)(drive->SectorsPerTrack ? drive->SectorsPerTrack : 63) * 512,
+		sector_size);
```

И обязательно добавить юнит-тест на 4Kn-геометрию (`SectorSize=4096, SectorsPerTrack=63`) с проверкой offset'а — сейчас `test_partition_plan` покрывает только 512.

---

#### M2. Фингерпринты MBR перепутаны: определитель ошибается на всех реальных бинарниках syslinux

`src/diskio.c:83-117`, таблица применения — `diskio.c:133-140`

```c
static bool mbr_is_syslinux(const uint8_t *s)
{
	/* syslinux' mbr.bin starts with 0xFA 0x33 0xC0 8E D0 ... */
	if (s[0] == 0xFA && s[1] == 0x33 && s[2] == 0xC0)
		return true;
	return memmem(s, 512, "Missing OS", 10) != NULL;
}
...
static bool mbr_is_isohybrid(const uint8_t *s)
{
	return s[0] == 0x33 && s[1] == 0xC0 && s[2] == 0xFA;
}
```

Комментарий фактически неверен: реальный `mbr.bin` начинается с `33 C0 FA` (`xor ax,ax; cli`), а не `FA 33 C0`. Зато `33 C0 FA` — это ровно то, что проверяет `mbr_is_isohybrid`. Настоящий `isohdpfx.bin` начинается с `33 ED 90`. Проверки **поменяны местами**.

Прогон определителей по реальным файлам из `syslinux-common` / `isolinux` (Debian 12):

| Бинарник | Что это на самом деле | first3 | Что скажет rufox |
|---|---|---|---|
| `/usr/lib/syslinux/mbr/mbr.bin` | SYSLINUX MBR | `33c0fa` | ❌ **ISOLINUX/isohybrid** |
| `/usr/lib/syslinux/mbr/altmbr.bin` | SYSLINUX alt MBR | `33c0fa` | ❌ **ISOLINUX/isohybrid** |
| `/usr/lib/syslinux/mbr/gptmbr.bin` | SYSLINUX GPT MBR | `33c0fa` | ⚠️ SYSLINUX (не GPT-вариант) |
| `/usr/lib/ISOLINUX/isohdpfx.bin` | настоящий isohybrid | `33ed90` | ❌ **unknown** |

4 из 4 неверно. Причина в том, что строковые маркеры тоже подобраны неточно:

```
mbr.bin      -> "Missing operating system."          (не "Missing OS")
altmbr.bin   -> "Missing operating system.", "Operating system load error."
gptmbr.bin   -> "Missing OS", "Multiple active partitions"
isohdpfx.bin -> "isolinux.bin missing or corrupt."
```

`"Missing OS"` встречается только в `gptmbr.bin`, поэтому `mbr_is_syslinux` срабатывает лишь на нём. Отдельная ловушка: `"Missing operating system"` входит и в `mbr_is_ms_sys` — то есть порядок таблицы (`syslinux` раньше `ms_sys`) является **смыслонагруженным**, но ничем не задокументирован.

Влияние ограничено диагностикой (`"%s has a %s Master Boot Record"` в логе), на результат записи не влияет. Но это пользовательская диагностика, по которой потом заводят багрепорты, и `PORTING.md §… ` прямо утверждает, что «offsets and magic bytes below are taken from ms-sys and syslinux' mbr.bin» — утверждение не соответствует коду.

**Патч:**

```c
 static bool mbr_is_syslinux(const uint8_t *s)
 {
-	/* syslinux' mbr.bin starts with 0xFA 0x33 0xC0 8E D0 and contains
-	 * the string "Missing OS" at a fixed offset. */
-	if (s[0] == 0xFA && s[1] == 0x33 && s[2] == 0xC0)
-		return true;
-	return memmem(s, 512, "Missing OS", 10) != NULL;
+	/* mbr.bin, altmbr.bin and gptmbr.bin all begin 33 C0 FA
+	 * (xor ax,ax; cli). Distinguish them by their message strings. */
+	if (!(s[0] == 0x33 && s[1] == 0xC0 && s[2] == 0xFA))
+		return false;
+	return memmem(s, 512, "Missing operating system.", 25) != NULL ||
+	       memmem(s, 512, "Missing OS", 10) != NULL;
 }
 
 static bool mbr_is_isohybrid(const uint8_t *s)
 {
-	return s[0] == 0x33 && s[1] == 0xC0 && s[2] == 0xFA;
+	/* isohdpfx.bin begins 33 ED 90 and carries its own error string. */
+	return (s[0] == 0x33 && s[1] == 0xED && s[2] == 0x90) ||
+	       memmem(s, 512, "isolinux.bin missing or corrupt.", 31) != NULL;
 }
```

Плюс: `gptmbr.bin` отличим по `"Multiple active partitions"`, `altmbr.bin` — по `"Operating system load error."`. И **добавить эти бинарники фикстурами в юнит-тест** — тогда регрессия станет невозможной. Сейчас ни один тест не покрывает `rfx_disk_analyze_mbr()`.

---

#### M3. Фолбэк `root=` из `/proc/cmdline` не работает на современных дистрибутивах и портит NVMe-имена

`src/dev.c:247-262`

```c
if (g_str_has_prefix(v, "/dev/")) {
	char *b = g_path_get_basename(v);
	size_t n = strlen(b);
	while (n > 0 && (isdigit((unsigned char)b[n-1]) || b[n-1] == 'p'))
		b[--n] = '\0';
	if (*b) mark_boot_disk(b);
```

Две проблемы:

**(a) Поддерживается только `root=/dev/…`.** Не обрабатываются `root=UUID=…`, `root=LABEL=…`, `root=/dev/disk/by-uuid/…`, `root=ZFS=…`, `root=PARTUUID=…`. А `root=UUID=` — это дефолт grub в Ubuntu/Debian/Fedora. То есть фолбэк, призванный страховать основной путь, на типичной системе молча не делает ничего.

**(b) Цикл снятия суффикса слишком наивен.** Он срезает и цифры, и буквы `'p'` без разбора:

| `root=` | полученное имя диска | правильно? |
|---|---|---|
| `/dev/sda2` | `sda` | ✅ |
| `/dev/nvme0n1p2` | `nvme0n1` | ✅ |
| `/dev/mmcblk0p1` | `mmcblk0` | ✅ |
| `/dev/nvme0n1` (диск целиком) | `nvme0n` | ❌ **срезана последняя цифра namespace** |
| `/dev/sdp` (реальное имя диска!) | `sd` | ❌ |

Основной путь (`/proc/mounts`, `dev.c:226-236`) эту задачу решает корректно — с проверкой префиксов `nvme`/`mmcblk`/`loop`/`md`/`dm-`. Фолбэк написан по-другому и с багами. Стоит вынести снятие суффикса в одну общую функцию и переиспользовать.

---

#### M4. Симлинки `/dev/disk/by-*` в `/proc/mounts` не разрешаются

`src/dev.c:206-220`

Канонизация через `g_file_read_link()` выполнена **только** для ветки `/dev/mapper/`. Все прочие `/dev/…` идут как `real = g_strdup(dev)` без `realpath()`. Если в `/proc/mounts` окажется `/dev/disk/by-uuid/1a2b-3c4d`, то `disk` станет `"disk/by-uuid/1a2b-3c4d"`, снятие хвостовых цифр его ещё и изуродует, и в `boot_disk_cache` попадёт строка, которая никогда не совпадёт с именем из `/sys/block`. Защита загрузочного диска в этом случае не сработает.

Практическая вероятность ниже, чем кажется: ядро обычно пишет в `/proc/mounts` каноническое имя блочного устройства (а для device-mapper — `/dev/mapper/…`, что как раз и обработано). Поэтому MEDIUM. Но ветка существует, и она молча производит мусор. Правильная защита — канонизировать всегда:

```c
-		} else {
-			real = g_strdup(dev);
-		}
+		} else {
+			g_autofree char *full = g_strdup_printf("/dev/%s", dev);
+			real = realpath(full, NULL);      /* g_canonicalize_filename() не
+			                                     разрешает симлинки — нужен именно realpath */
+			if (real != NULL) {
+				char *b = g_path_get_basename(real);
+				g_free(real);
+				real = b;
+			}
+		}
```

---

#### M5. Windows To Go недостижим из оркестрации

`PORTING.md §7` описывает Windows To Go как работающую функциональность (wimlib + самостоятельная UEFI-загрузка через `bootmgfw.efi` → `\EFI\Boot\bootx64.efi` на ESP). Но `job.c` не вызывает `rfx_wintogo_create()` **нигде**:

```
$ grep -n 'wintogo\|WIN_TO_GO' src/job.c
732:            if (j->Image.is_windows_img && o->image_option != RFX_IMOP_WIN_TO_GO)
```

Единственное упоминание — отрицательная проверка. Единственный потребитель API — `main_cli.c:187` (печать «supported / not supported» в отчёте).

Это значит, что даже после возврата `src/wintogo.c` в архив функциональность не заработает: её никто не вызывает. Либо в `job.c` fehlt ветка WTG, либо она не попала в архив вместе с `wintogo.c`. Уточнить при перезаливке. Заодно: `RfxOptions.esp_mountpoint` (`rufox.h:350`) документирован как «Where the ESP ended up being mounted … Windows To Go needs it» — стоит проверить, что он действительно заполняется.

---

#### M6. Комментарий в `rufox.h` описывает `DeviceId` неверно — и на этом держится фильтрация

`src/rufox.h:222`

```c
char     DeviceId[64];		/* stable id (by-id symlink tgt) */
```

`src/dev.c:551`

```c
g_strlcpy(out->DeviceId, name, sizeof(out->DeviceId));   /* name = basename(devpath), т.е. "sdb", "dm-0" */
```

В поле лежит **имя ядра**, а не цель by-id-симлинка. Код при этом верен: фильтр `rfx_dev_is_filtered()` (`dev.c:673-676`) проверяет префиксы именно имени ядра:

```c
if (g_str_has_prefix(d->DeviceId, "dm-"))   return true;
if (g_str_has_prefix(d->DeviceId, "md"))    return true;
if (g_str_has_prefix(d->DeviceId, "ram"))   return true;
if (g_str_has_prefix(d->DeviceId, "zram"))  return true;
```

Я специально проверил это, потому что при значении «by-id symlink target» все четыре условия не срабатывали бы никогда и LVM/RAID/zram предлагались бы пользователю на стирание. К счастью, это не так.

Но опасность в другом: контрибьютор, который «исправит» `dev.c:551` в соответствии с заголовочным комментарием (начнёт класть туда реальный by-id), **молча сломает всю фильтрацию виртуальных устройств**, и ни один тест этого не поймает. Комментарий надо привести к коду, а лучше — переименовать поле в `KernelName` и завести отдельное `StableId`, если оно нужно. И добавить тест на `rfx_dev_is_filtered()` для `dm-0`/`md0`/`zram0`.

---

#### M7. Флаг `-f`: README и GUI обещают одно, CLI делает другое

README: «`rufox` — CLI с теми же флагами (`-i`, `-f`, `-x` совместимы с rufus.exe)».
`src/main_gui.c:39`: «Rufus' own switches are -i/-f/-x/-g; we keep -i and -f compatible».

Фактически в `cliopts.c:123` принимается только `-F` (заглавная):

```
$ ./build/rufox info -i fixture.iso -f FAT32
Unknown option '-f'
```

При этом GUI объявляет именно строчную: `{ "filesystem", 'f', 0, G_OPTION_ARG_STRING, &opt_fs, … }` (`main_gui.c:27`). То есть один и тот же проект принимает `-f` в GUI и `-F` в CLI, а сериализация аргументов для helper'а (`cliopts.c:188`) отправляет `-F`.

Расхождение GUI/CLI по одному и тому же флагу — это путаница в скриптах и документации. Либо добавить `-f` как синоним `-F` в CLI (что согласуется с заявленной совместимостью с rufus.exe), либо поправить README и комментарий в `main_gui.c`.

---

#### M8. `RfxCancel.ref_count` объявлен, но не используется — кандидат в use-after-free

`src/cancel.c`

```c
struct _RfxCancel {
	volatile gint requested;
	gint          ref_count;      /* инициализируется в 1 и больше не трогается */
};
...
void rfx_cancel_free(RfxCancel *c) { g_free(c); }   /* без декремента */
```

Поле явно задумывалось под подсчёт ссылок, но API получился однопоточно-владелческим. Токен создаётся в одном потоке, опрашивается из worker'а (`rfx_cancel_requested()` вызывается в циклах записи/верификации/zeroing), освобождается в первом. Если GUI-поток освободит токен, пока worker ещё внутри `write_with_retry()` (например, отмена + закрытие окна), получим чтение освобождённой памяти.

Само чтение атомарно и корректно (`g_atomic_int_get`/`g_atomic_int_set` — здесь всё правильно; `volatile` при этом избыточен). Проблема в жизненном цикле объекта.

Рекомендация: либо доделать refcounting (`g_atomic_int_inc` / `g_atomic_int_dec_and_test` в `_free`), либо убрать поле и явно задокументировать контракт владения («токен живёт не дольше `rfx_job_run()`»). Проверить по `src/ui/engine.c`, когда он появится, — именно там GTask будет держать токен.

---

#### M9. `tools/crash.c` никогда не компилируется

Файл устанавливает обработчики `SIGSEGV`/`SIGABRT`/`SIGBUS` через `__attribute__((constructor))` и печатает backtrace — полезная вещь для полевого отладчика. Но в `meson.build` он **не упомянут ни в одном таргете**:

```
$ grep -n 'crash' meson.build
(пусто)
```

Значит, обработчик не устанавливается никогда, и при падении на машине пользователя проект не получит backtrace. Либо добавить файл в `core_src` (тогда конструктор покроет все бинарники), либо оформить как явный `-Dcrash_handler` option.

Дополнительно: `fprintf(stderr, …)` внутри сигнального обработчика не async-signal-safe. `backtrace_symbols_fd()` — допустим, `fprintf` — нет; его стоит заменить на `write(2, …)`. `SA_RESETHAND` установлен верно.

---

### LOW / косметика

| # | Место | Проблема |
|---|---|---|
| L1 | `diskio.c:393` | `uprintf("\nWrote %s in %.1f s (%s/s)", …, rfx_human_rate(...))`, а `rfx_human_rate()` (`rutil.c:148`) уже дописывает `/s`. В логе: **`(1.1 GB/s/s)`**. Фикс: `"(%s)"`. |
| L2 | `format.c` / `job.c` | Двойное оборачивание текста ошибки. Наблюдаемое: `Could not format /tmp/mbr2.img@64512: Could not format /tmp/mbr2.img@64512: as FAT32: mkfs.vfat: failed whilst writing FAT` — префикс повторён, внутренняя склейка разорвана. |
| L3 | `diskio.c:59`, и др. | `rfx_set_error(code, errno, …)` после цикла ретраев с `g_usleep()`/логированием: `errno` уже затёрт. Эмпирически наблюдал `(Resource temporarily unavailable)` для ошибки, которая на самом деле была `ENOSPC`. Фикс: сохранять `errno` сразу после неудачного `pwrite`. |
| L4 | `bootloaders.c` | `Using SYSLINUX  MBR` — двойной пробел: строка версии пуста. Rufus печатает `Using SYSLINUX 6.04 MBR`. |
| L5 | `rproc.c:31` | Комментарий «Children must never inherit a half-open block device of ours» над `unsetenv("LD_PRELOAD")` — не соответствует коду (наследие другой правки). |
| L6 | `data/rufox-helper.policy` | `<vendor_url>https://github.com/pbatard/rufus</vendor_url>` — polkit показывает это как «поставщика» действия **rufox**. Кредитовать апстрим правильно и нужно (лицензионное требование GPL и просто честно), но `vendor_url` должен вести на репозиторий порта, а ссылка на Rufus — в `<description>`/LICENSE/README. |
| L7 | `diskio.c:334` | `uint64_t target = src_size < dst_size ? src_size : dst_size;` — мёртвый код: строкой ниже `src_size > dst_size` отсекается ошибкой, так что `target` всегда равен `src_size`. |
| L8 | `diskio.c:400-402` | Лишнее forward-объявление `rfx_disk_verify_image()` непосредственно перед её определением. |
| L9 | `job.c` + `diskio.c:327-331` | Сжатый образ отклоняется **после** подтверждения записи, внутри `rfx_disk_write_image()`, и с кодом `ERR_UNSUPPORTED_FS` (семантически про файловые системы, а не про сжатие). `PORTING.md §10` ограничение признаёт, но UX стоит улучшить: блокировать выбор сжатого образа сразу в `info`/GUI и завести отдельный код ошибки. |
| L10 | `dev.c:667-680` | `rfx_dev_is_filtered()` не учитывает `MountPoint`. Незагрузочный диск со смонтированными данными будет размонтирован (`job.c:478`) и стёрт без отдельного предупреждения. Поведение совпадает с Rufus, но на Linux стоит выводить в диалог подтверждения список mountpoint'ов и размеры разделов. |
| L11 | `dev.c:308-311` | `uint64_t nsect = 0; if (ioctl(fd, BLKGETSIZE64, &nsect) == 0 …) d->DiskSize = nsect;` — `BLKGETSIZE64` возвращает **байты**, а переменная названа `nsect`. Присваивание корректно (`DiskSize` в байтах), но имя вводит в заблуждение. |
| L12 | Архив | `src/cancel(1).c` — дубль; `.DS_Store`; `__MACOSX/`; два `.desktop.download`. Стоит добавить `.gitignore`/`.gitattributes` и собирать релизный tarball через `git archive`. |

---

## 5. UI и юзабилити

**Полноценно проверить невозможно: весь UI-слой отсутствует в архиве** (§1.1). `rufox-gui` не собирается ни при каких опциях — `meson.build:142` перечисляет четыре несуществующих файла, а `main_gui.c:11` включает отсутствующий `src/ui/rufox-gui.h`. Из 71 строки `main_gui.c` следует, что вся реальная форма (device combo, boot selection, partition scheme, target system, file system, cluster, label, advanced-чекбоксы, лог, START/CANCEL, прогресс) живёт в `app.c`/`window.c`/`engine.c`.

Что можно сказать по доступному:

**Сделано правильно**
* Структура формы в README повторяет диалог Rufus один-в-один: Drive Properties → Format Options → Advanced → Log → START/CANCEL. Правильная декомпозиция, и «Image option» корректно показан только для Win-образов.
* `main_gui.c`: `setlocale(LC_ALL, "")` до инициализации GTK; `GApplication`-жизненный цикл; при ошибке разбора опций контекст и приложение освобождаются до `return 2` — утечек нет.
* `PORTING.md §8` описывает правильный механизм маршалинга прогресса из worker-потока в главный: `GAsyncQueue` + idle-source как аналог `PostMessage`. Это корректный GTK-идиом (прямые вызовы GTK из worker'а были бы гонкой).
* Разделение на `rufox-gui` / `rufox` (CLI) / `rufox-helper` (privilege) — грамотная архитектура. Общий парсер `cliopts.c` с одной функцией сериализации/десериализации аргументов для helper'а исключает рассогласование семантики между процессами — это хорошее инженерное решение, и оно честно описано в `PORTING.md §9`.

**Баг в единственном доступном GUI-файле**
* `main_gui.c:17-19, 41-56`: `opt_iso`, `opt_device`, `opt_fs` разбираются из командной строки, а затем **только освобождаются** (`g_free`) — в приложение не передаются. Используются лишь `opt_verbose` и `opt_dry_run`. Переменные объявлены `static`, то есть другой translation unit не может их прочитать. Получается, что `rufox-gui -i image.iso -d /dev/sdb` преselect не делает. Либо это мёртвый код, который надо удалить, либо не хватает вызова вида `rufox_app_set_preselection(app, opt_device, opt_iso, opt_fs)` — проверить, когда `ui/*.c` появятся.

**Что надо будет проверить после перезаливки** (чек-лист под ваш же сценарий «UI и юзабилити»)
1. Блокировка всех контролов на время записи; CANCEL доступен всегда.
2. Каскад зависимостей как в Rufus: смена Partition scheme → пересчёт допустимых Target system; смена File system → пересчёт допустимых Cluster size (правило «FAT32 требует ≥65525 кластеров» уже есть в ядре — `rfx_format_cluster_allowed`, и оно покрыто тестом `test_cluster_gate`; важно, чтобы UI его использовал, а не давал выбрать заведомо нерабочую комбинацию).
3. `Image option` показывается только при `is_windows_img`.
4. Диалог подтверждения выводит `rfx_job_describe()` (функция есть, `job.c:262`) **и** список разделов/mountpoint'ов целевого диска (см. L10).
5. Прогресс: проценты, скорость и ETA; `progress_render` в CLI (`main_cli.c:73-97`) уже корректно обрабатывает `progress < 0` (indeterminate) и clamp'ит в [0,1] — в GUI нужно то же.
6. Перечитывание списка устройств при hotplug (GUdev `uevent` при `HAVE_GUDEV`, иначе опрос раз в 2 с — `PORTING.md §1`) и сохранение выбора, если устройство не исчезло.
7. Что происходит при выдёргивании флешки во время записи: ошибка должна быть внятной, а не «зависшим» прогрессом.
8. Локализация: `RUFOX_LOCALEDIR` в конфигах есть, `gettext-domain="rufox"` в policy есть, но `gettext()`/`_()` в исходниках я не встречал — похоже, строки пока не обёрнуты для перевода. Проверить.
9. `data/rufox.gschema.xml` присутствует — убедиться, что `glib-compile-schemas` вызывается на install и что ключи схемы совпадают с тем, что читает `app.c`.
10. Доступность: подписи (`aria-label`-эквиваленты) для комбобоксов, поскольку форма плотная.

---

## 6. Оценка по пунктам вашего запроса

| Пункт | Оценка | Обоснование |
|---|---|---|
| **Архитектура и качество кода** | **8.5 / 10** | Модульная структура повторяет апстрим, имена enum'ов и структур совпадают с Rufus (`RfxImgReport` ≙ `RUFUS_IMG_REPORT`) — логи сверяются с багрепортами апстрима. 0 warnings при `-Wall -Wextra`. Аккуратный meson с feature-опциями и двойными фолбэками для зависимостей. `PORTING.md` — образцовый документ: объясняет *почему*, а не *что*. Минус за мёртвый код (`crash.c`, `ref_count`, `target=MIN`, тавтологичный guard) и расхождения комментариев с кодом (M6, L5). |
| **Корректность работы** | **6 / 10** | То, что работает, работает превосходно: 30/30 моих побайтовых проверок разметки, 48/48 ваших тестов, DD идентичен по sha256, offset 64512, type 0x0C, `mbr.bin` байт-в-байт, патчинг конфигов точный. Но H1–H5 — это дефекты в самом критичном месте (путь записи и подтверждение успеха): фантомный GPT после DD, «успех» при `EIO`, верификация-пустышка, игнорирование отказа блокировки. Для утилиты, чьё главное обещание — «Reliable», это существенный минус. |
| **UI и юзабилити** | **не оцениваемо** | Слой отсутствует в архиве. По косвенным признакам (структура формы, идеоматичный маршалинг прогресса, трёхпроцессная архитектура) задумано правильно. Найден один баг в доступной части — CLI-преselect в GUI не передаётся (§5). |
| **Собираемость** | **0 / 10 как поставлено**<br>**9 / 10 после дозагрузки недостающих файлов** | `meson setup` падает на строке 67. С заглушками — полная сборка без единого предупреждения, все три бинарника линкуются, тесты проходят. То есть проблема **не в коде, а в комплектации архива**. |

---

## 7. Приоритетный план

**Немедленно (блокеры поставки)**
1. Перезалить архив из рабочего дерева, убедившись что на месте: `src/wintogo.c`, `src/ui/{rufox-gui.h,engine.c,app.c,window.c}`, `data/rufox-gui.desktop`. Убрать `cancel(1).c`, `.DS_Store`, `__MACOSX/`, `.download`-файлы.
2. Прогнать `meson setup build && ninja -C build && meson test -C build && DESTDIR=/tmp/stage ninja -C build install` **с включённым GUI** — сейчас `install` упадёт на отсутствующем desktop-файле, и это всплывёт у первого же пакера.

**До первого релиза (корректность записи)**
3. H1 — `tail_len = buf_len` (`diskio.c:200`).
4. H2 — вызвать `clear_mbr_gpt` в DD-ветке (`job.c`, перед `write_image`).
5. H3 — отказ `flock` → `FAIL(ERR_DEVICE_BUSY, …)` (`job.c:465`).
6. H4 — убрать `EIO` из «не фатальных» (`rutil.c:245`).
7. H5 — `BLKFLSBUF` (или `O_DIRECT`) перед верификацией (`diskio.c:413`).
8. Добавить регрессионные тесты на H1/H2: записать GPT, перезаписать DD-образом меньшего размера, и убедиться, что в последних 64 секторах нет ни `EFI PART`, ни ненулевых записей. Тест на файл-цели — ровно та инфраструктура, которая у вас уже есть.

**Следующим заходом**
9. M1 — починить guard и `bytes_per_track`, добавить тест на 4Kn-геометрию.
10. M2 — починить фингерпринты, положить реальные `mbr.bin`/`gptmbr.bin`/`altmbr.bin`/`isohdpfx.bin` в фикстуры и покрыть `rfx_disk_analyze_mbr()` тестом.
11. M3/M4 — привести фолбэк `root=` и канонизацию `/proc/mounts` к общему со снятием суффикса знаменателю; поддержать `UUID=`/`LABEL=`/`PARTUUID=`.
12. M5 — подключить `rfx_wintogo_create()` к `job.c` либо честно убрать WTG из списка возможностей в README/`PORTING.md`.
13. M6 — исправить комментарий про `DeviceId` и добавить тест на фильтрацию `dm-`/`md`/`zram`.
14. M7 — согласовать `-f`/`-F` между GUI, CLI и README.
15. M8/M9 — доделать или удалить `ref_count`; встроить `crash.c` в сборку.
16. Пройтись по L1–L12 (в основном однострочные правки; L1, L2, L3, L4 заметны пользователю в логе).

---

## 8. Приложение: как воспроизвести мои проверки

Все инструменты, которыми я пользовался, лежат в рабочей папке:

| Файл | Назначение |
|---|---|
| `verify_image.py` | побайтовый аудит MBR/GPT/FAT32 после `write` (30 проверок) |
| `tail_probe.py` | карта ненулевых секторов в хвосте диска, поиск следов backup GPT |
| `buildcheck/harness_clear.c` | изолированный вызов `rfx_disk_clear_mbr_gpt()` — доказательство H1 |
| `buildcheck/src/wintogo_stub.c` | заглушка отсутствующего `wintogo.c` — только для диагностики сборки |
| `buildcheck/` | копия дерева с правками `meson.build` для сборки; **оригинал `rufox-src/` не изменён** |

```sh
# зависимости стенда (Debian 12)
apt install build-essential meson ninja-build pkg-config \
    libglib2.0-dev libgtk-4-dev libadwaita-1-dev libgudev-1.0-dev libudisks2-dev \
    libblkid-dev libcdio-dev libiso9660-dev libudf-dev libparted-dev \
    xorriso isolinux syslinux-common dosfstools mtools

# сборка (в копии, с заглушками)
cd buildcheck && meson setup build -Dgui=disabled && ninja -C build

# тесты с фикстурой
sh rufox-src/tests/make_fixture.sh /tmp/rufox-fixture.iso
./buildcheck/build/rufox-tests          # 48 checks, 0 failure(s)

# сквозной цикл и аудит
truncate -s 4G /tmp/disk.img
./buildcheck/build/rufox write -d /tmp/disk.img -i /tmp/rufox-fixture.iso \
        -S GPT -T UEFI -F FAT32 -y -v
python3 verify_image.py /tmp/disk.img   # RESULT: 30/30 checks passed
```

Ограничение стенда: `/dev` здесь — tmpfs без loop-устройств и без udev, поэтому запись на реальную флешку не проверялась. Все сценарии прогнаны на файл-целях через тот самый механизм `file@@offset`, который реализован в `rtarget.c`/`format.c`. Это покрывает разметку, mkfs, копирование дерева, патчинг конфигов и DD, но **не** покрывает поведение udev/udisks2 на живом железе, автомонтирование, `BLKRRPART` и реальную запись на деградирующий носитель (а значит, H4/H5 подтверждены анализом кода и отсутствием `BLKFLSBUF`, а не экспериментом на железе).
