# cs2_gc

Локальный эмулятор Game Coordinator (GC) для **Counter-Strike 2**: порт [csgo_gc](https://github.com/mikkokko/csgo_gc) (Mikko Kokko, BSD-2) с CS:GO на CS2.

GC — сервис Valve, который отвечает за инвентарь, лоадаут, кейсы, наклейки и ранги. cs2_gc перехватывает трафик игры к GC и отвечает сам, прямо в процессе игры. Инвентарь хранится локально в `cs2_gc\inventory.txt`.

> [!NOTE]
> **Статус: работает в игре (проверено 2026-11-06, запуск рядом с `cs2.exe`).** Инвентарь, экипировка, скины в матче с ботами, кейсы, аренда кейсов на 7 дней, наклейки, брелоки, нейм-теги, хранилища, музыка, Armory Pass (покупка) и веб-редактор работают. Контракты обмена и мод-лоадер CSForge (свои скины и музыка) тоже проверены в игре. Кейсы Kilowatt/Gallery/Fever/Dead Hand и сувенирные наборы 2025 проверены self-test'ом. Снятие брелоков, Armory (активация пропуска, звёзды, награды) и активация нового хранилища проверены в игре 2026-11-07. Вкладка Mods в редакторе, замена звуков, обмен StatTrak, терминалы, уровень с XP, недельный дроп, слэбы, сувенирное качество, турнирные товары и inspect-ссылки пока проверены только self-test'ом и в браузере. Чего не хватает, см. в тир-листе «Что осталось» ниже.

## Безопасность (VAC)

- Игра **всегда** стартует с `-insecure`: лаунчер сам перезапускается с этим ключом, если его нет, и отказывается запускаться с `-trusted`. В insecure-режиме VAC не загружается, на официальные и VAC-серверы зайти нельзя.
- Экспорт `BSecureAllowed` в `cs2_gc.exe` возвращает `false`, хук `InitGameServer` (`SteamGameServer010`–`015`) снимает флаг secure. Даже локальный сервер не может стать «secure».
- **Файлы CS2 не меняются.** cs2_gc только читает `engine2.dll`/`tier0.dll`/`steam_api64.dll` и `pak01_dir.vpk`. Всё, что он пишет (конфиг, инвентарь, лог), лежит в папке `cs2_gc\` рядом с `cs2_gc.exe`.
- Первые запуски всё равно лучше делать **со второго Steam-аккаунта**.

## Как это работает

```
cs2_gc.exe (лаунчер, вне папки игры)
 ├─ перезапуск с -insecure, если ключа нет
 ├─ ищет CS2 (libraryfolders.vdf / -cs2path / своя папка)
 ├─ грузит game\bin\win64\tier0.dll, затем engine2 через tier0!Plat_LoadModule
 ├─ InstallGC():
 │   ├─ путь к steamclient64.dll берёт из реестра (HKCU\Software\Valve\Steam\ActiveProcess)
 │   ├─ inline-хук steamclient64!CreateInterface → прокси ISteamClient023
 │   │     ├─ SteamGameCoordinator001 → наш GC (ClientGC / ServerGC)
 │   │     ├─ SteamUser023 / SteamGameServer015 / SteamUtils010 / SteamMatchMakingServers002 / SteamFriends017 (rich presence → XP за матч)
 │   └─ хуки SteamAPI_RegisterCallback / RunCallbacks (для GCMessageAvailable_t)
 └─ Source2Main(hInst, 0, cmdline, nShow, "<game\bin\win64>", "csgo") — так же, как это делает cs2.exe
```

Всё, что касается версии CS2, восстановлено из файлов установленной игры (только чтение, игра не запускалась):

| Что | Откуда | Инструмент |
|---|---|---|
| `protobufs/*.proto` (GC-схемы CS2) | дескрипторы, вшитые в `client.dll` | `tools/dump_protos.py` |
| Раскладка vtable `ISteamClient023`, `ISteamUser023`, `ISteamGameServer015` | flat-API в `steam_api64.dll` | `tools/vtable_from_flat.py` |
| Прокси-классы Steam-интерфейсов | `steam_old/*.h` | `tools/steamproxygen.py` |
| Сигнатура `Source2Main`, порядок загрузки | дизассемблирование `cs2.exe` | — |
| `items_game.txt` | `game\csgo\pak01_dir.vpk` (читается на лету) | `cs2_gc/vpk.cpp`, `tools/vpk_extract.py` |
| Таблица имён GC-сообщений | `protobufs/*.proto` | `tools/gen_message_names.py` |

После апдейта CS2 обычно хватает перезапустить `dump_protos.py` и `gen_message_names.py` и пересобрать.

## Чем CS2 отличается от CS:GO (и что уже портировано)

| CS:GO (csgo_gc) | CS2 (cs2_gc) |
|---|---|
| `SteamClient020`, `SteamUser022`, `SteamGameServer014` | `SteamClient023` (убраны GameSearch/UnifiedMessages/AppList/MusicRemote), `SteamUser023` (+`GetAuthTicketForWebApi`), `SteamGameServer015` (`GetAuthSessionTicket` с `SteamNetworkingIdentity`) ✅ |
| `csgo.exe` → `launcher.dll!LauncherMain` | `cs2.exe` → `engine2.dll!Source2Main(..., baseDir, "csgo")` ✅ |
| `SteamAPI_Init` для поиска steamclient | путь из реестра, Steam API до игры не трогаем ✅ |
| `items_game.txt` отдельным файлом | внутри `pak01_dir.vpk` ✅ |
| Экипировка: `k_EMsgGCAdjustItemEquippedState` + SO type 43 | `k_EMsgGCAdjustEquipSlotsManual/Shuffle` (`CMsgAdjustEquipSlots`) + SO type 3 `CSOEconEquipSlot` ✅ (нужна проверка в игре) |
| Кейсы: struct `k_EMsgGCUnlockCrate` | protobuf `k_EMsgGCOpenCrate` (`CMsgOpenCrate`) ✅ |
| Нейм-теги и хранилища: struct `k_EMsgGCNameItem` / `NameBaseItem` / `RemoveItemName` | те же id, но protobuf `CMsgModifyItemStringAttr` (`tool_item_id`, `subject_item_id`, строка в `attr_value`; `attr_def` только у описания). Новое хранилище активируется именем с виртуальным Name Tag (`0xF0000000000004B0`, def 1200) вместо инструмента (проверено в игре) |
| — | ключи кейсов после 2022 года: `price_sheet.txt` снят в 2022-м, и ключей Kilowatt, Gallery и Fever (`community_33_key`…`community_35_key`) в нём не было, поэтому окно кейса не предлагало купить ключ и открыть. Теперь они в прайсе по цене ключа Revolution (не проверено в игре) |
| — | аренда кейса на 7 дней (`for_rental`): все обычные скины кейса с атрибутами `is rental` (297) и `expiration date` (183), без ножей и перчаток; истёкшие убираются сами ✅ |
| — | контракты обмена: запрос protobuf `CMsgCraftItemsRequest` (`k_EMsgGCCraft`), ответ по-прежнему struct `k_EMsgGCCraftResponse`; 10 → следующая редкость той же коллекции, 5 тайных → нож/перчатки кейса коллекции (StatTrak → только нож), float по нормализованной формуле ✅ |
| — | сувенирные наборы 2025+: к сувениру крепится брелок с моментом матча на этой карте (`match_highlight_reel_keychain`, `highlight_reels`, атрибут 314) ✅ |
| Наклейки: id + wear | + поворот, масштаб, смещение x/y (атрибуты 278–289) ✅ |
| — | брелоки: `keychain slot 0 *` (299–321), `ApplyKeychain`, `keychain_definitions` в схеме, открытие капсул брелоков ✅ |
| — | снятие брелока (`InventoryAPI.RemoveKeychain`, тоже через `k_EMsgGCApplySticker`): заряды в SO type 15 `CSOAccountKeychainRemoveToolCharges`, брелок возвращается в инвентарь, Remove Keychain Tool Pack (4950) даёт 3 заряда (проверено в игре) |
| — | Armory: SO type 6 `CSOAccountXpShop` (звёзды и треки пропусков), активация пропуска (`UseItem` 1354), покупка (`k_EMsgGCCStrike15_v2_ClientRedeemMissionReward`) по витрине `seasonaloperations` → `xpshop` (проверено в игре) |
| — | обмен StatTrak-счётчиков (`k_EMsgGCStatTrakSwap`, `CMsgApplyStatTrakSwap`, инструмент 1324): счётчики меняются местами, если у обоих предметов один тип счётчика (убийства или MVP) (не проверено в игре) |
| — | терминалы Genesis и Dead Hand: распечатка через `OpenCrate` → активный терминал (качество 14, аренда на 24 часа), предложения через тот же `OpenCrate` с номером предложения (`UseToolWithIntArg`), предложение лежит в терминале как в хранилище, покупка через магазин, каталог коллекции и XP за него (SO type 20/21, `VolatileItemClaimReward`) (не проверено в игре) |
| — | уровень и XP хранятся в `inventory.txt`, XP за матч с ботами и на community-серверах (по rich presence клиента, с недельным бонусом) и за каталог терминала; недельный дроп: первое повышение уровня за неделю даёт 4 предмета, из них берутся 2 (SO type 4 `CSOAccountItemPersonalStore`, `ClientRedeemFreeReward`) (не проверено в игре) |
| — | медаль за службу на 40 уровне: `k_EMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin` (9172) в обе стороны. Проверка (`InventoryAPI.RequestPrestigeCoinCheck`) приходит пустой, ответ называет медаль текущего года (`prestige coin <год>`, потом `... level 2` до 6; если года ещё нет в схеме — самого нового) и `upgradeid` медали этого года, если она уже есть. Получение приходит с `defindex`: новая медаль (origin 9 Earned, неподтверждённая) или та же медаль следующего уровня на месте, уровень снова 1 с 0 XP, в ответе `upgradeid` = медаль, клиент её показывает. Без ответа проверка крутилась бесконечно (не проверено в игре) |
| — | Sticker Slab: наклейка → брелок 37 с наклейкой (`keychain slot 0 sticker`), уведомление `EncapsulateSticker` (не проверено в игре) |
| Сувениры без турнирных атрибутов | качество 12, событие и команды (137, 139, 140), золотые наклейки организатора, команд и игрока (не проверено в игре) |
| — | inspect-ссылки своих предметов (`Client2GCEconPreviewDataBlockRequest`), пропуск зрителя → монета и жетоны, жетон → сувенирный набор (не проверено в игре) |
| `CMsgSO*.owner` | только `owner_soid` ✅ |
| Ранги: comp / wingman / danger zone | comp / wingman / **premier** (CS Rating) ✅ |
| AppID 4465480 + правка `csgo/steam.inf` | всегда 730, папка игры не трогается ✅ |
| Удаление кэша иконок в `csgo/resource/flash` | убрано ✅ |

## Сборка

Нужны Visual Studio 2022 (C++), встроенные CMake и vcpkg. Отдельно ставить ничего не нужно.

```bat
build.cmd
```

Первая сборка идёт долго (~20–30 мин): vcpkg собирает protobuf, abseil, mbedtls и libwebsockets. Результат появляется в `release\`:

```
release\cs2_gc.exe
release\cs2_gc\config.txt, inventory.txt, items_extra.txt, price_sheet.txt
```

Повторная сборка не перезаписывает твои `config.txt` и `inventory.txt`.

## Офлайн self-test

Проверяет без запуска игры и Steam всё, что можно проверить без рантайма: чтение VPK, разбор схемы предметов CS2, открытие всех кейсов (включая новые и сувенирные 2025), `inventory.txt`, сборку SO cache, сообщения `CSOEconEquipSlot`, аренду, контракты обмена, снятие брелоков, Armory, обмен StatTrak, терминалы, уровни и недельный дроп, слэбы, сувениры, inspect-ссылки, пропуски зрителя и турнирные товары магазина, сборку модов CSForge с вкладкой Mods и прокси `SteamGameServer015`. Файлы CS2 только читает, пишет только в указанную папку.

`--serve <секунды>` поднимает только сервер редактора поверх `<папка данных>\inventory.txt`, чтобы проверить страницу без игры. Если игра с cs2_gc уже запущена и держит порт 13001, положи в папку данных `config.txt` с `"inventory_editor_port" "13002"` и открывай http://127.0.0.1:13002/.

```bat
build\tests\cs2_gc_selftest.exe "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" "%TEMP%\cs2_gc_test"
```

В папку данных нужно положить `items_extra.txt` и `price_sheet.txt` из `examples\`. Результат на текущей версии CS2 (сокращённо):

```
ok   466 crates with loot lists
ok   9320 openings ok, 0 failed
ok   Kilowatt Case (4904) opens, 13 knives/gloves in the rare list
ok   Fever Case (7007) opens, 52 knives/gloves in the rare list
ok   Austin 2025 Inferno souvenir package (5120) -> charm 36, highlight 129 of 92 inferno reels
ok   rental gave 9 items (notification lists 9)
ok   10 mythical Kilowatt -> legendary Kilowatt only (3 of 3 outcomes seen)
ok   5 covert Kilowatt -> knife (def 526 quality 3, kukri is 526)
ok   Fever knives: Doppler is one finish of 7 (14.5% of 20000, expected ~14.3%)
ok   inventory trade up: 10 inputs consumed, 1 legendary crafted item left (1 items, origin 4, rarity 5)
ok   SO cache has KeychainRemoveToolCharges (type 15, 10 charges) and XpShop (type 6, 0 stars)
ok   the charm is back in the inventory as a keychain item with its pattern seed
ok   Armory: the redeem_ids the game sends find their entries (case 2, charms 3, collection 4 stars)
ok   Armory: all 11 entries give an item (case, charm...), 29 stars spent, 11 left
ok   5 offers, one stored in the terminal at a time with a price, numbered 1 2 3 4 -5 (final)
ok   level 3: the week's care package, 4 items to pick 2 (SO type 4)
ok   sticker slab: sticker 8557 + slab -> charm of keychain 37 showing it, both used up, keychain_seal
ok   souvenir: quality 12, Austin 2025 teams 134 vs 127, gold stickers 8685 8573 8677 9326 (32 teams known)
ok   Mods tab: low switched off with priority 7 in mod.txt (comment kept), marked changed
ok   InitGameServer (slot 0) reaches steam with the secure flag removed (flags 1)
PASSED (0 failures)
```

Списков ножей и перчаток (`*_unusual`) в публичном `items_game.txt` нет, они лежат в `items_extra.txt`. После выхода нового кейса недостающие списки печатает `tools\gen_unusual_lists.py`.

Набор P250 «Рентген» (def 4668) не открывался ни в csgo_gc, ни в cs2_gc: в публичной схеме есть только ссылка на его лут-лист `crate_xray_p250_lootlist`, без содержимого. Теперь он прописан в `items_extra.txt`. Кроме него, без содержимого остались только промо-наборы DreamHack 2013 и EMS 2014 и подарочные паки.

## Запуск

| Как запускаешь | Что получаешь |
|---|---|
| **Steam → Play** | обычная CS2 с VAC, как раньше: cs2_gc в неё не встраивается, папка игры не тронута |
| **`run_cs2_gc.cmd`** (или `release\cs2_gc.exe`) | CS2 с локальным GC, всегда `-insecure` (оффлайн, боты, свои сервера) |

`cs2_gc.exe` — отдельная программа, а не замена `cs2.exe` и не ярлык на него.

Как играть с cs2_gc:
1. Steam запущен.
2. Запусти `run_cs2_gc.cmd` и **не закрывай его окно**, пока играешь. Ключи передаются дальше в игру (`run_cs2_gc.cmd -novid -console`). Путь к CS2 задаётся вверху скрипта (`CS2_ROOT`).
3. Лог: консоль CS2 (строки `[GC]`) и `release\cs2_gc\gc_log.txt`.

**Проверено в игре:** CS2 стартует, только если `cs2_gc.exe` лежит в `game\bin\win64\` рядом с `cs2.exe`. Поэтому `run_cs2_gc.cmd` кладёт туда копию **только на время игры** и запускает её с `-gcdata release\cs2_gc`: конфиг, инвентарь, лог и редактор остаются в папке проекта. Когда игра закрывается, скрипт сам вызывает `cleanup_cs2_folder.cmd`.

## Следы в папке CS2 и VAC

cs2_gc не меняет файлы игры, реестр и параметры запуска Steam. Всё, что от него может остаться в папке CS2:
- `game\bin\win64\cs2_gc.exe` — копия лаунчера на время игры;
- `game\bin\win64\cs2_gc\` — папка данных, если exe запускали вручную без `-gcdata`;
- `game\bin\win64\cs2_gc_*.mdmp` — краш-дампы, которые игра называет по имени exe.

`run_cs2_gc.cmd` удаляет всё это после выхода из игры. Если окно закрыли раньше времени или игра упала, запусти **`cleanup_cs2_folder.cmd`**: он удалит только эти файлы, проверит, что ничего `cs2_gc*` не осталось, и не тронет файлы игры. Перед игрой на официальных серверах просто убедись, что он пишет «CS2 folder is clean».

VAC проверяет то, что загружено и работает в процессе игры. Лишний exe, который лежит в папке и при обычном запуске из Steam не запускается, в процесс игры не попадает. Но как именно VAC сканирует диск, Valve не публикует, поэтому надёжнее перед легитной игрой держать папку чистой, для этого и сделана очистка.

## Редактор инвентаря

Редактор mikkokko (`csgo_invedit`), переделанный под CS2, лежит в `editor\` и при сборке копируется в `release\cs2_gc\editor\`. Страницу отдаёт сам GC:

1. Запусти игру через `run_cs2_gc.cmd`.
2. Открой **http://127.0.0.1:13001/** (или `open_editor.cmd`) и нажми **Connect**.
3. Вкладка Catalogue: поиск по-русски и по-английски, кнопка «Add to inventory». Изменения сразу появляются в игре и сохраняются в `inventory.txt`.
4. Вкладка Mods: моды CSForge в порядке загрузки. Галочка включает и выключает мод, поле Priority меняет порядок; это сохраняется в `mod.txt` мода (остальное в файле, включая комментарии, не трогается) и применяется при следующем запуске игры (мод с изменениями помечен «restart the game to apply»). Там же видно число файлов, есть ли предметы и тексты и какие файлы мода перекрывает мод с большим приоритетом.

Правка предметов инвентаря: в панели выбранного предмета блок «Edit item» — редкость (от Stock до Contraband), качество (Normal, Genuine, Vintage, ★ Unusual, Unique, StatTrak™, Souvenir; звезда у ножа — это качество Unusual), износ и сид для раскрашенных, StatTrak со счётчиком (атрибуты 80/81; у оружия переключатель заодно меняет качество Unique ↔ StatTrak™) и нейм-тег (атрибут 111). «Save changes» шлёт готовую команду протокола `modify` с полным описанием предмета: GC применяет её через `UpdateFromDesc` (экипировка остаётся), отправляет игре и сохраняет в `inventory.txt`. Исключение — StatTrak у хранилища (1201): у него нет модели, на которую клиент вешает счётчик, и игра падает (access violation под Panorama). Редактор такой переключатель не показывает, GC снимает атрибуты 80/81 с хранилищ и при правке, и при загрузке. У медалей и кейсов модели есть, StatTrak на них не роняет игру.

Оформление в духе системных приложений: стеклянная верхняя панель с переключателем вкладок и индикатором подключения, скруглённые карточки с подсветкой цвета редкости за картинкой, инспектор со сгруппированной таблицей свойств, переключатели вместо галочек, тёмная и светлая тема по настройке системы (`editor\style.css`; размеры карточек совпадают с `grid.js`: высота 220 px, ширина от 160 px).

Порт страницы и WebSocket меняется настройкой `"inventory_editor_port"` (по умолчанию 13001), страница сама подключается к тому порту, с которого открыта.

Каталог собран из файлов CS2: 2059 предметов, 14 161 вариант (1478 скинов оружия, 556 ножей, 94 перчатки, 11 615 наклеек, 143 брелока, 102 набора музыки, граффити, нашивки). Иконки — картинки Steam Community из данных [ByMykel/CSGO-API](https://github.com/ByMykel/CSGO-API), для них нужен интернет. После апдейта CS2 каталог пересобирается одной командой:

```bat
python tools\gen_invedit_catalog.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --lang russian
```

Офлайн-редактор dricotec изменений не требует: формат `inventory.txt` у cs2_gc такой же. Открывай в нём `release\cs2_gc\inventory.txt`, когда игра закрыта, иначе GC перезапишет файл.

## CSForge (мод-лоадер)

CSForge — мод-лоадер cs2_gc: свои скины, музыкальные наборы, звуки, названия и замена файлов игры (текстуры, звуки, модели) без изменения папки CS2. Включается настройкой `"mods" "1"` в `config.txt`, по умолчанию выключен. Работает только вместе с cs2_gc, то есть в `-insecure`. Проверено в игре 2026-11-06 на демо-моде: свой скин с иконкой и видом, своя музыка в меню. Включать, выключать и упорядочивать моды можно во вкладке Mods редактора инвентаря.

### Как устроен мод

```
cs2_gc\mods\<папка мода>\
  mod.txt                  "mod" { "name" "..." "author" "..." "version" "..." "enabled" "1" "priority" "0" }
  items_game.txt           "items_game" { "paint_kits" { ... } "music_definitions" { ... } ... }
  localization.txt         "lang" { "Tokens" { "PaintKit_my_skin_Tag" "Мой скин" } }
  localization_russian.txt то же, но только для одного языка
  files\...                файлы с игровыми путями: свои новые или замена игровых
```

Все файлы необязательны. Папки, имя которых начинается с `_` или `.`, пропускаются.

- **`files\`** — скомпилированные файлы (`.vtex_c`, `.vcompmat_c`, `.vsnd_c`, `.vmdl_c`…) по тем же путям, что в VPK игры (пути не зависят от регистра). Если два мода кладут один и тот же путь, побеждает мод с большим `priority`, а в лог пишется, кто кого заменил.
- **`items_game.txt`** — разделы, которые дописываются в конец таких же разделов игровой схемы. Если id уже есть у игры или у мода, загруженного раньше, запись пропускается с предупреждением в лог: дубли роняют клиент. Изменить существующую запись игры нельзя.
- **`localization.txt`** — токены добавляются во все языки из `"mod_languages"` (по умолчанию `english russian`). Существующие токены здесь *заменяются*, так что можно переименовать что угодно в игре.
- Порядок загрузки: по `priority` (меньше — раньше), при равном — по имени папки.

В `cs2_gc\gc_log.txt` строки `CSForge:` показывают список модов, конфликты, пропущенные id и итоговый VPK.

### Как работает

1. При старте cs2_gc склеивает включённые моды: берёт `items_game.txt`, `csgo_<язык>.txt` из `pak01_dir.vpk` игры (или из `files\` мода, если он кладёт файл целиком) и добавляет разделы модов по порядку загрузки. Результат вместе с `files\` всех модов упаковывается в `cs2_gc\mods\_build\csforge_dir.vpk` (VPK v2, данные внутри `_dir`-файла).
2. Перехват `LoadLibraryExW` ждёт загрузки `filesystem_stdio.dll` и подменяет в её таблице `AddSearchPath`. Когда engine2 добавляет `game\csgo`, CSForge ставит перед ним `mods\_build\csforge.vpk` (пути `GAME` и `MOD`).
3. Схема GC читает `items_game.txt` из того же VPK, поэтому GC знает предметы модов (редкость, износ).

### Что выяснено по файлам игры

- В CS2 раскраска — это не цвета в `items_game.txt`, как в CS:GO, а скомпилированный ассет: `.vcompmat_c` (поле `composite_material_path`) с текстурами `items/assets/paintkits/...vtex_c` и иконки `panorama/images/econ/default_generated/weapon_<оружие>_<раскраска>_{light,medium,heavy}_png.vtex_c`.
- Музыкальный набор — запись в `music_definitions` плюс `soundevents/music/<имя>/game_sounds_music.vsndevts_c` и треки `sounds/music/<имя>/*.vsnd_c`.
- **Файлы из VPK приоритетнее отдельных файлов** при любом порядке путей поиска. Отдельный `items_game.txt` в пути, стоящем первым, игра даже не открывает. Поэтому всё упаковывается в свой VPK.
- `filesystem_stdio` сверяет каждый VPK с `bin\win64\vpk.signatures` по имени: имя из списка (`pak01`) из папки, которой там нет, — фатальная ошибка `Failed to load file (unexpected)`; `pakNN` не из списка — `pakxx=off`; любое другое имя не из списка подключается без подписи. Поэтому VPK называется `csforge_dir.vpk`.
- `VFileSystem017::AddSearchPath` — слот 31, аргументы `(путь, ID пути, 0 = в начало / 1 = в конец, 0, 0)`. engine2 вызывает её для каждого пути из `gameinfo.gi`.
- Клиент читает схему через `CEconItemSystem::ParseItemSchemaFile("scripts/items/items_game.txt")` (`KeyValues::LoadFromFile`, путь `GAME` или `MOD`).
- Первая раскраска (`"0"`, `default`) — шаблон для остальных. Своя раскраска перед ней роняет клиент, поэтому записи модов добавляются в конец раздела.

### Демо-мод

`tools\make_demo_mod.py` собирает `mods\cs2gc_demo` из `examples\mods\cs2gc_demo`. В него входят раскраска 20001 на материале «Наследия», иконки и старая тема CS:GO в меню (копии файлов игры для локальной проверки, не для распространения). `--add-item` добавляет в инвентарь AK-47 с этой раскраской, `--enable` включает `"mods"`.

```bat
python tools\make_demo_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --add-item --enable
```

### Свой музыкальный набор из mp3

`tools\make_music_mod.py` делает мод из одного трека без Workshop Tools:

```bat
python tools\make_music_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --mp3 musics\track.mp3 --name "Мой набор" --kit theverkkars_01 --add-item
```

- **Что делает:** заменяет все треки существующего набора (`--kit`, имя из `music_definitions`; все наборы и какие из них можно заменить — в [music_kits.md](music_kits.md)) и переименовывает его через локализацию. `--add-item` кладёт этот набор в инвентарь, если игра закрыта. Совсем новый набор пока не сделать: ему нужны свои скомпилированные события звука (`game_sounds_music.vsndevts_c`, KV3 со сжатием LZ4) и запись в `soundevents_manifest.vrman_c`.
- **Как режет трек:** каждый слот (меню, начало раунда, MVP, заложенная бомба, 10 секунд до конца…) получает длину трека этого слота у оригинального набора. Слот вырезается из твоего трека с момента `--at <слот>=<секунды>` (по умолчанию 0), а если трек короче — повторяется по кругу (подходит для зацикленных треков). Меню получает песню целиком. Режется по MP3-кадрам (26 мс), без перекодирования.
- **Нарезка мышкой:** `--cutter` открывает в браузере страницу с волной трека (`tools\music_cutter.html`, локальный сервер на 127.0.0.1). Выбираешь отрезок в списке, кликаешь по волне — начало отрезка ставится туда и сразу играет ровно то, что будет в игре (с переходом через конец трека). «Собрать мод» собирает мод и сохраняет точки в `<трек>.slots.json` рядом с треком. Следующие запуски без `--cutter` берут точки из этого файла.
- **Формат `.vsnd_c`:** заголовок ресурса (тип 4: блоки `RED2` и `DATA`), за ним обычные MP3-кадры. В `DATA`: `+0` — упакованный формат (биты 0–16 — частота, 17–23 — формат: 0 PCM16, 1 MP3, 24–25 — каналы), `+8` — число сэмплов, `+12` — длительность (float), `+28` — размер аудио. Заголовок берётся от трека оригинального набора, эти поля перезаписываются.
- **Наборы формата 5** (новые: CS:GO от Valve, Half-Life: Alyx, почти все с id 78+): описание звука в блоке `CTRL` (KV3), блок `DATA` пустой. Игра читает и тип 4, и тип 5 (в `isoxo_01` они перемешаны), поэтому скрипт кладёт на их место файлы типа 4 с заголовком от трека другого набора. В игре ещё не проверено.
- **Входной файл:** MP3 44,1 кГц стерео — как есть. m4a, wav, wma и другие перегоняются в MP3 через ffmpeg, а если его нет — встроенными кодеками Windows (`tools\convert_audio.ps1`, WinRT `MediaTranscoder`).

### Свои звуки (выстрелы, шаги, бомба, интерфейс)

`tools\make_sound_mod.py` заменяет любой звук игры своим файлом (mp3, wav, m4a…), без Workshop Tools:

```bat
python tools\make_sound_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --list ak47
python tools\make_sound_mod.py --cs2 "..." --data release\cs2_gc --audio shot.mp3 --sound "sounds/weapons/ak47/ak47_0*" --length 0.6 --gain -3
python tools\make_sound_mod.py --cs2 "..." --data release\cs2_gc --remove "sounds/weapons/ak47/*"
```

- `--list слово…` показывает звуки игры (всего ~20 000 `.vsnd_c`: голоса, музыка, окружение, оружие, игрок, интерфейс) с типом, форматом, каналами и длиной.
- `--sound` принимает маску и повторяется. `--start`/`--length` вырезают кусок твоего файла, `--gain` меняет громкость в дБ. Всё пишется в мод `mods\sounds` (`--mod` — другой), повторные запуски добавляют туда же, `--remove` убирает замену.
- Звуки игры — 16-битный PCM, поэтому и замена делается в PCM: заголовок типа 4 от звука игры (шаги), свои частота, каналы, число сэмплов и размер. Моно по умолчанию, чтобы звук слышался в 3D. Если у оригинала типа 4 два канала, будет стерео. У типа 5 число каналов из заголовка не прочитать, поэтому моно, `--stereo`/`--mono` задают явно.
- Декодирование: ffmpeg, а без него встроенные кодеки Windows. WinRT пишет WAV только 48 кГц стерео, поэтому частота пересчитывается в 44,1 кГц, а каналы сводятся в Python. В игре ещё не проверено.

### Своя заставка

Заставка при запуске (логотип COUNTER-STRIKE 2) — ролик, который скрипт `intromovie` берёт из `file://{resources}/videos/intro.webm`: это `game\csgo\panorama\videos\intro.webm` (VP9 3840×2160 с дорожкой Opus) и `intro720p.webm` рядом (VP8 1280×720). Ролики лежат в папке игры отдельными файлами, не в VPK, а файл из VPK побеждает отдельный, поэтому мод кладёт свои по тем же путям в `files\panorama\videos\`. Звук логотипа идёт отдельно (событие `UIPanorama.IntroLogo`, скорее всего `sounds/ui/cs2_logo_master` или `sounds/ui/panorama/cs2_logo`), его меняет `make_sound_mod.py`.

```bat
python tools\make_intro_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --test
python tools\make_intro_mod.py --data release\cs2_gc --video my_intro.webm
python tools\make_intro_mod.py --data release\cs2_gc --remove
```

- `--test` ставит вместо заставки ролик самой игры (фон меню «Сломанного клыка», `op9_main`), чтобы за один запуск проверить подмену.
- `--video` — свой ролик: WebM (VP9 или VP8) кладётся как есть, остальное перегоняется через ffmpeg (VP9 1080p и VP8 720p). `--video720` — отдельный файл для 720p.
- С `-novid` заставки нет вообще. В игре ещё не проверено: если видеоплеер читает ролики с диска мимо файловой системы, подмена не сработает.

### Встроенные исправления интерфейса

При включённых модах CSForge при каждом старте ещё и правит скрипты Panorama самой игры (`"panorama_fixes" "1"` в `config.txt`, по умолчанию включено). Скомпилированный скрипт `.vts_c` — это ресурс с блоками `RED2`, `DATA` (сам JavaScript простым текстом) и `STAT` (контрольной суммы текста в нём нет). Код вставляется перед якорем в текущий скрипт из `pak01_dir.vpk`, таблица блоков пересчитывается, файл кладётся в `csforge_dir.vpk`. Поэтому после апдейта игры не подсовывается её старая копия; если якорь пропал, исправление пропускается со строкой в логе.

- **Кейс во время матча.** Без ключа окно кейса показывает панель покупки ключа, а во время матча её нет (по задумке Valve контейнеры в матче не открываются: `SFUI_InvError_OpenCrate_PauseMenu`). Оставалось окно без единой кнопки, даже «Закрыть», и закрыть его можно было только выходом из игры. Теперь, если ключа нет и купить его нельзя, окно открывается в режиме «только посмотреть», как у арендованного кейса (`inspect_only`, `only_close_btn`), с кнопкой «Закрыть», а в матче ещё и с сообщением игры «Нельзя открывать контейнеры во время матча». С ключом в инвентаре всё как раньше. В игре ещё не проверено.

### Апгрейдер

Тоже встроенное исправление интерфейса (нужны `"mods" "1"` и `"panorama_fixes" "1"`): вкладка «Апгрейдер» в инвентаре рядом с остальными, и пункт «Апгрейд» в меню скина (ПКМ), который кладёт скин в ставку и открывает вкладку. Слева ставка (до 10 скинов, «+ Добавить скин» показывает скины инвентаря с ценами, дорогие сверху; ножи и перчатки тоже можно ставить), в середине колесо и «ставка → цель» в деньгах, справа цели — конкретный скин в конкретном износе (FN/MW/FT/WW/BS) с ценой и шансом, от дешёвых к дорогим (фильтры по редкости, типу, «то же оружие», StatTrak™, множители x1.5–x10, страницы).

- **Шанс — по реальным ценам:** шанс = стоимость ставки / цена цели (фиолетовый за 50 ₽ в ширп за 100 ₽ — 50%; скины ставки складываются). Цель должна стоить больше ставки: ширп за 120 в синий за 20 нельзя (было бы 600%), такие цели просто не показываются, а уже выбранная пишет «Цель должна быть дороже ставки». Цена скина зависит от износа, StatTrak™ и сувенирности — как на маркете; выигранный скин получает float внутри выбранного износа (и диапазона самой раскраски). Нож из выигрыша StatTrak, если выбрана цель StatTrak™. Множитель xN оставляет цели не дешевле ставки × N (x2 — шансы от 50% и ниже). При неудаче ставка сгорает. Скин без цены на маркете в своём износе поставить нельзя (окно скажет почему).
- **Откуда цены:** `https://market.csgo.com/api/v2/prices/RUB.json` — открытый список без ключа (~27 тыс. предметов, минимальная цена предложения). Скачивается при старте игры (WinHTTP, ~2,5 МБ), если сохранённый `cs2_gc\prices_RUB.json` старше 12 часов; без интернета берётся старый файл. Валюта и срок — в конфиге: `"upgrader_prices" "RUB"` (или `USD`, `EUR`; `"off"` — старое правило 1:10 по редкости), `"upgrader_prices_hours" "12"`. Название для маркета собирается из английских текстов игры (`resource/csgo_english.txt`: `item_name` оружия и `description_tag` раскраски, `★`, `StatTrak™`, `Souvenir`, износ) — на текущем списке цену нашли все 2100 скинов, 9104 из 9258 износов (остальных просто нет в продаже). Фазы допплеров и камни (Ruby, Sapphire) на маркете называются одинаково «Doppler» и стоят в списке одну цену. Без списка вообще (нет интернета и файла) цены выдумываются из редкости — 10^ступень, то есть прежние 1:10.
- **Колесо:** кольцо, на нём снизу оранжевая дуга размером с шанс (`clip: radial(50% 50%, начало, ширина)`, как кольцо прогресса Armory), в маленьком круге процент, стрелка. После ответа GC стрелка делает 8 оборотов за 6 с, быстро в начале и с замедлением к концу (ease-out cubic), с тиками (`UIPanorama.XP.Ticker` каждые 30°, не чаще 70 мс), и останавливается в дуге при выигрыше или вне её при проигрыше. Крутится покадрово из скрипта: `transition` между двумя `rotateZ` идёт коротким путём, целые обороты теряются.
- **Сюрприз до конца:** GC бросает сразу (иначе колесу некуда целиться), но выигранный скин не виден, пока стрелка не встала. Скрипт инвентаря показывает окно «новый предмет» на каждое обновление инвентаря, кроме времени, пока открыт попап кейса или контракта, — вкладка на время вращения шлёт то же событие `CapabilityPopupIsOpen` (true, затем false через 0,8 с после остановки: тогда игра сама показывает скин со звуком). Счётчик новых предметов на кнопке инвентаря (`MainMenuInvAlert`) на это время держится на прежнем значении. Если ставки пропали, а нового скина нет, вкладка ждёт ещё 0,3 с: скин приходит отдельным сообщением.
- **Как устроено:** CSForge при старте собирает каталог из схемы игры (2100 скинов: всё раскрашиваемое из списков выпадения, с ценами по износам: обычная, StatTrak, сувенир) и вставляет модуль `CsgcUpgrader` в начало `mainmenu_inventory`, а его установку — в `_Init` после создания вкладок игры: своя кнопка в группе `inv-top-nav` и панель с классом `inv-category`, переключение — родным `NavigateToTab`. Пункт меню (`item_context_entries`) находит панель `InventoryMain` и зовёт хук из её `Data()`. Выбор уходит в GC через `InventoryAPI.SetItemAttributeValueAsync` (`k_EMsgGCModifyItemAttribute`; клиент пропускает любой атрибут со `stored_as_integer` 1): «upgrade threshold» = номер скина × 16 + износ × 2 + StatTrak + 1 на каждый скин ставки, затем «upgrade level» = их число. Бросок делает GC (`Inventory::Upgrade`) по тем же ценам, что видит окно, в логе — строка `Upgrader:` со стоимостью ставки и цели.
- **Замечания:** фильтр `InventoryAPI.SetInventorySortAndFilters` сначала ждёт категорию снаряжения (`rifle,is_rental:false`, как у поиска и хаба мейджора), одни флаги вроде `not_base_item` дают пустой список — скины для ставки собираются по `secondary`, `smg`, `rifle`, `heavy`, `melee`, `clothing_hands` и подкатегориям из `InventoryAPI.GetCategories/GetSubCategories` с ножами и перчатками (список пишется в консоль: `[csgc upgrader] inventory categories`). V8 в Panorama без нормальной локали, `localeCompare` там бросает RangeError — сортировка обычным сравнением. Ошибка в одной части вкладки пишется в её строку статуса и в консоль (`[csgc upgrader]`), остальные части рисуются. Списки категорий отдают и базовые пушки (faux id, по одной на оружие — 34 в `secondary`/`smg`/`rifle`/`heavy`); если ставить нечего, вкладка пишет словами, сколько настоящих скинов и почему они не подходят (стандартные, без цены на маркете, уже в ставке), базовые не считаются. Строковый литерал модуля разбит на куски: MSVC не берёт литералы больше 16 КБ.

### Сложность ботов

В меню CS2 выбора сложности нет: выпадающий список `BotDifficultyDropdown` остался в разметке `mainmenu_play` (пункты #SFUI_BotDifficulty1–5, обработчик `GameTypesAPI.SetCustomBotDifficulty` и `player_botdifflast_s`), но у него класс `hidden`, и скрипт его не открывает. Поэтому сложность задаёт конфиг режима: `cfg/gamemode_casual.cfg` — `bot_difficulty 1`, соревновательный и DM — `2`. `tools\make_bot_mod.py` делает мод, который это меняет:

```bat
python tools\make_bot_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc
python tools\make_bot_mod.py --cs2 "..." --data release\cs2_gc --difficulty hard --modes casual deathmatch
python tools\make_bot_mod.py --cs2 "..." --data release\cs2_gc --remove
```

- **Как:** каждый режим после своего конфига выполняет `gamemode_<режим>_server.cfg` (список `exec` в `gamemodes.txt`) — файл для владельцев серверов, которого в игре нет. Мод кладёт такие файлы в `files\cfg\`, файлы игры не заменяются. Сложность — `easy`, `normal`, `hard`, `expert` (по умолчанию) или 0–3, `--modes` — только некоторые режимы.
- **Две переменные.** `server.dll` выбирает уровень бота из `bot_difficulty` (0–3) только при `game_online`; в офлайн-матче с ботами — из `custom_bot_difficulty`: 3 обычные, 4 сложные, 5 эксперты, ниже — лёгкие. Мод ставит обе, поэтому в матче с ботами ввод `bot_difficulty 3` в консоли сам по себе ничего не даёт, нужен `custom_bot_difficulty 5`.
- Применяется со следующего запуска игры (CSForge собирает VPK при старте), в консоли появляется `CSForge bots: ...`. В игре ещё не проверено: если этой строки нет, игра не выполняет cfg из VPK.

### Свои ассеты

Текстуры, материалы и звуки компилируются бесплатными CS2 Workshop Tools (`resourcecompiler`). Это официальный DLC Valve, но он ставится в папку CS2. Готовые `_c` файлы кладутся в `files\` мода. id своих раскрасок — выше 10100, музыкальных наборов — выше 110.

Если игра падает с модами, поставь `"mods" "0"`. Дамп падения сохраняется в `release\cs2_gc\crashes\` (`cleanup_cs2_folder.cmd` переносит его из папки игры).

## Что осталось (тир-лист)

Составлен 2026-11-06 по коду, логу реальной игры (`gc_log.txt`) и схеме предметов текущей CS2. **S** — делать первым, **D** — последним. Оценки — реальное время работы плюс сколько раз нужно запустить игру для проверки. Для ориентира: весь cs2_gc от первой строки до мод-лоадера CSForge — около 4 часов работы (из ~10 часов, остальное — ожидание лимитов и запуски).

### Сделано из тир-листа (2026-11-06)
- **S:** кейсы Kilowatt, Gallery, Fever и терминал Dead Hand: списки ножей/перчаток в `items_extra.txt` (генератор `tools/gen_unusual_lists.py`).
- **S:** прокси `SteamGameServer015`.
- **A:** контракты обмена, включая 5 тайных → нож/перчатки (проверено в игре).
- **A:** сувенирные наборы Austin 2025 и Budapest 2025 (брелок с моментом матча). У Austin Ancient моментов в схеме нет, сувенир выпадает без брелока.
- Шансы Doppler как в игре: фазы и камни ножа делят шанс одной отделки (раньше, как и в csgo_gc, каждая фаза и камень считались отдельной отделкой, и Doppler выпадал больше чем в половине случаев). Камень (Ruby, Sapphire, Black Pearl, Emerald) весит 1/5 фазы — точного соотношения Valve не публикует. Действует и на кейсы, и на контракты.
- Мод-лоадер CSForge (проверен в игре), см. «CSForge (мод-лоадер)».

### Сделано из тир-листа (2026-11-07)
- **A: снятие брелока** (проверено в игре). Заряды — SO type 15 `CSOAccountKeychainRemoveToolCharges`, хранятся в `inventory.txt` (блок `"account"`), новый инвентарь начинает с `"keychain_remove_charges"` из конфига (10). Брелок возвращается в инвентарь со своим узором/моментом матча, стоковое оружие, на котором был только брелок, исчезает. Remove Keychain Tool Pack (4950, есть в магазине) даёт 3 заряда. `InventoryAPI.RemoveKeychain` шлёт `ApplySticker` с `sticker_item_id` = служебный id базового предмета 65 «Remove Keychain Tool» (`0xF000000000000041`) и слотом 0. На всякий случай GC понимает и пустой слот наклейки на оружии с брелоком; такие запросы пишутся в лог (`ApplySticker without a sticker: ...`).
- **A: Armory** (проверено в игре). SO type 6 `CSOAccountXpShop`: звёзды (`redeemable_balance`) и треки пропусков (`xp_tracks`). Пропуск (1354) активируется сразу полным: +40 звёзд, трек 40 × 300 XP (константы `StoreAPI.GetXpShopStarXp` = 300, `GetXpShopMaxTrackLevel` = 40, `GetXpShopMaxTracks` = 5 из `client.dll`). Покупка `ClientRedeemMissionReward`: клиент присылает `redeem_id` = CRC-32 от `item_name` позиции (например, `crate_community_35` → 1025083006). Позиции берутся из витрины `seasonaloperations` → `"redeemable_goods" "xpshop"` (`KeyValueRead` сливает одинаковые ключи, поэтому этот раздел разбирается из текста), `lootlist:…` разыгрывается как кейс, остальное — предмет по имени. `Client2GcAckXPShopTracks` убирает законченные треки.
- **CSForge A:** вкладка Mods в редакторе, замена любых звуков (`make_sound_mod.py`), замена музыкальных наборов формата 5 (в игре ещё не проверено).

Номера SO-типов зашиты в RTTI `client.dll`: `.?AV?$CProtoBufSharedObject@VCSOAccountXpShop@@$05@GCSDK@@` — `$05` = 6, `$0P@` = 15 (MSVC кодирует числовые параметры шаблона так: `$0` и цифра = значение − 1, или hex-буквы A–P и `@`). Так же видны остальные: 2 PersonaDataPublic, 3 EquipSlot, 4 ItemPersonalStore, 5 RentalHistory, 7 GameAccountClient, 16 RecurringMission, 19 XpShopBids, 20 VolatileItemOffer, 21 VolatileItemClaimedRewards, 39 RecurringSubscription, 40/41 SeasonalOperation, 45 EconCoupon, 46 QuestProgress.

### Сделано из тир-листа (2026-11-07, вторая часть, в игре ещё не проверено)
- **B: обмен StatTrak.** `InventoryAPI.UseTool` с выбранной парой шлёт `k_EMsgGCStatTrakSwap` (`CMsgApplyStatTrakSwap`, на всякий случай принимается и старый struct с теми же тремя id). Счётчики (атрибут 80) меняются местами, если тип счётчика (81) у обоих одинаковый: убийства с MVP музыкального набора не смешиваются. Инструмент 1324 расходуется, набор из двух (4088) есть в магазине.
- **B: терминалы** (Sealed Genesis Terminal 5176, Sealed Dead Hand Terminal 5181; оба теперь продаются в магазине). Разобрано по `popup_offers_laptop*.js` и `client.dll`:
  - распечатка — `OpenCrate` на запечатанном терминале → активный терминал: та же def, качество 14 «volatile», аренда на 24 часа, уведомление `crate_unlock`;
  - следующее предложение — `InventoryAPI.UseToolWithIntArg(терминал, терминал, n)`, это тоже `OpenCrate` (`tool = subject = терминал`, `points_remaining` = номер, `volatile_limit` = лимит цены; 5318008 в том же аргументе означает аренду кейса). Предложение лежит в терминале как предмет в хранилище, с ценой в атрибуте 316 `purchase price`; номер — в атрибуте 169 `quest points remaining`, у пятого, последнего, он отрицательный. Ответ — `casket_contents` (1012) с id терминала. Запрос после последнего предложения закрывает терминал;
  - повторное открытие — `k_EMsgGCVolatileItemLoadContents` (2536), тоже `CMsgOpenCrate`;
  - покупка — через магазин строкой `<def>(<id терминала>)`; предмет сохраняет id, получает сертификат первого владельца (317), терминал исчезает. Если в строке покупки нет id терминала, предложение узнаётся по цене;
  - каталог коллекции: просмотренные предложения — SO type 20 `CSOVolatileItemOffer` (faux id `0xF…` | paint << 16 | def, золотой предмет — `GetFauxItemIDFromDefAndPaintIndexUB1(def, 1, 3)`); «Get Bonus XP» за полный ряд редкости — `k_EMsgGCCStrike15_v2_VolatileItemClaimReward` с `CSOVolatileItemClaimedRewards`, отметка в SO type 21.

  Неточности: цены придуманы (от редкости, износа и StatTrak), сколько XP даёт каталог, неизвестно (тут 500–2000 за ряд, 3000 за золотой предмет), и после «Get Bonus XP» кнопка остаётся в состоянии «Claiming XP…» до следующего открытия: какое уведомление ждёт клиент (`xpgrant`), не найдено. Наклейки Cologne 2026 (`volatile container` = 3, `VolatileShopSubscribe`) — другой механизм, не сделан.
- **Исправлено: обычные скины не принимал контракт обмена** (так же было в csgo_gc). Качество предмета из кейса бралось из префаба оружия в схеме, а там `"item_quality" "normal"` (0, стоковая пушка). Рецепты контрактов требуют `"*quality" "unique,tournament"` (StatTrak — `strange`), поэтому годились только StatTrak, красные (5 → нож) и скины с качеством 4 из редактора. Теперь раскрашенное оружие из кейсов, контрактов, терминалов и недельного дропа получает `unique` (4), а уже выданные с качеством 0 исправляются при загрузке инвентаря (только огнестрел, у ножей и перчаток `unusual`). Скорее всего, по той же причине такие скины не клались в хранилище — проверить в игре.
- **Исправлено: недельный дроп.** Предложенные предметы создавались с `inventory` 0, а для клиента это «новый предмет»: он показывал все четыре в окне новых предметов вместо выбора 2 из 4. Теперь у них обычная позиция, а прячет их атрибут 277. Выбор открывает главное меню само (`mainmenu.js` `_CheckRankUpRedemptionStore`: `PersonalStore.redeemable_balance` > 0 и новый `generation_time` относительно `cl_redemption_reset_timestamp`), поэтому, пока выбор не сделан, GC при каждом запуске ставит новый `generation_time`, и окно появляется снова. Уже созданный дроп чинится при загрузке.
- **B: уровень, XP и недельный дроп.** Уровень и XP теперь в блоке `"account"` `inventory.txt` (`player_level`/`player_cur_xp` из конфига только для нового инвентаря), клиент получает их через `MatchmakingGC2ClientHello` и SO type 2. XP приходит за конец матча: сервер шлёт `k_EMsgGCCStrike15_v2_MatchEndRunRewardDrops` с `xp_progress_data` (это шлёт только выделенный сервер; матчи с ботами и на community-серверах считаются по rich presence, см. «XP за матчи» ниже), и за каталог терминала. 5000 XP на уровень, на 40-м шкала начинается заново. Первое повышение уровня за неделю (неделя с среды 00:00 UTC) даёт недельный дроп: 4 предмета (кейсы Kilowatt, Gallery, Fever или скины коллекций `set_timed_drops_*`, Train 2025, Overpass 2024), из которых берутся 2 — SO type 4 `CSOAccountItemPersonalStore`, выбор `ClientRedeemFreeReward` → уведомление `free_reward_redeemed`. Пока не выбраны, предметы помечены атрибутом 277 `free reward status` и лежат вне рюкзака (`inventory` 0); остальные два исчезают.
- Еженедельные миссии не сделаны (см. B ниже).

### B — полезно
| Что | Почему | Оценка |
|---|---|---|
| Еженедельные миссии | Шаблон миссии из `CMsgRecurringMissionSchema.mission_templates` клиент превращает в JS-объект как есть (ключи `id`, `name`, `loc_description`, `goal_points`, `xp_reward`, `string_tokens`, `mapgroup` из данных, `client.dll` 0x797910), а прогресс в матче считает сервер (`GameStateAPI.GetActiveQuestID`, user message `CS_UM_RecurringMissionSchema` 387). Формат байтов шаблона известен только GC Valve, нужен дамп настоящего ответа или разбор парсера; `RequestRecurringMissionSchedule` пока молча игнорируется | ~1–2 часа разбора, 2–3 запуска |

### Сделано из тир-листа (2026-11-07, XP за матчи, в игре ещё не проверено)
- **B: XP за матч с ботами и на community-серверах.** Свой отчёт о конце матча (`MatchEndRunRewardDrops`) `server.dll` шлёт только на выделенном сервере: после «Game Over» его отправку закрывает `engine->IsDedicatedServer()` (`CEngineServer`, слот 34 `Source2EngineToServer001`; рядом `sv_matchend_drops_enabled` и флаг прерванного матча), поэтому в матче с ботами XP не было. Теперь GC следит за rich presence клиента: `client.dll` сам вызывает `SteamFriends017::SetRichPresence` в любом матче (`game:state` game/lobby, `game:mode`, `game:map`, `game:score` «[ свои : чужие ]», `game:server` offline/community/watch), а прокси `SteamFriends017` передаёт эти ключи в ClientGC.
  - Конец матча — выход в лобби, смена карты, сброс счёта (новый матч на той же карте) или закрытие игры. Матчи короче минуты, демо и GOTV (`watch`) не считаются.
  - Как в CS2, XP только за доигранный матч. Доигран, если сервер сам перешёл дальше (сменил карту или начал матч заново), или если на момент выхода счёт показывает конец по `gamemode_*.cfg` игры: обычный и ретейки — 8 побед или 15 раундов, соревновательный и Premier — 13 или 24, Wingman — 9 или 16, Deathmatch — 10 минут. Остальные режимы — только когда сервер перешёл дальше (community-серверы после матча меняют карту). Ушёл раньше — в логе `Match: left before the end`.
  - Базовый XP как в CS2: соревновательный и Premier — 30 за выигранный раунд, Wingman — 15. Deathmatch и обычный в CS2 платят по очкам игрока, а их в rich presence нет, поэтому по времени: 15 XP в минуту (до 200) и 10 в минуту (до 350; так же арсенал, ретейки и режимы community-серверов).
  - Недельный бонус CS2 по XP, набранному за неделю (с среды): ×4 до 4500, ×2 до 7500, ×1 до 11 167, дальше ×0,175. Счётчик недели — `xp_earned`/`xp_earned_week` в `inventory.txt`.
  - Если выделенный сервер с нашим GC всё же прислал свой отчёт, этот матч второй раз не оплачивается.
  - В `SteamFriends017`, который использует `client.dll`, 80 методов: в старом SDK из репозитория их 76, а `client.dll` вызывает и `RequestEquippedProfileItems` (слот 76), и `GetProfileItemPropertyString` (78). Первая версия прокси без них роняла игру при старте. Переходники flat API в `steam_api64.dll` CS2 дают другие номера (у них нет `SetPersonaName` и `GetUserRestrictions`), но игра их не вызывает, ориентир — вызовы в `client.dll`. Если после апдейта Steam игра начнёт падать при старте, `"match_xp" "0"` в `config.txt` выключает прокси.
  - Отличие от CS2: в DM и обычном режиме XP зависит от времени, а не от очков игрока.

### Сделано из тир-листа (2026-11-07, C и D, в игре ещё не проверено)
- **C: «на что играешь» в профиле.** `rank_if_win/lose/tie` теперь заполнены: у званий они равны текущему (офлайн звание не меняется), у Premier — ±125 рейтинга (типичный матч), это видно в окне «Ставки матча» (`match_stakes.js`, `predictedRankingIfWin/IfLoss`).
- **C: inspect-ссылки** (`+csgo_econ_action_preview S…A…D…`): `Client2GCEconPreviewDataBlockRequest` → `CEconItemPreviewDataBlock` из своего инвентаря, износ — точным float (его биты, как в ссылках). Ссылки на чужие предметы и с маркета показать нечем, это пишется в лог. Новые «самодостаточные» ссылки CS2 (с данными предмета внутри) клиент разбирает сам.
- **C: наклейка → слэб.** Sticker Slab (4000, `sticker_display_case`, `display case keychain id` 37) + наклейка в любом порядке приходит как `ApplySticker`, а `InventoryAPI.RemoveKeychain(наклейка, 0)` — как он же с базовым предметом 65. Получается брелок 1355 брелока 37 с наклейкой в атрибуте 321 `keychain slot 0 sticker`, редкостью наклейки (у турнирной — и качеством), уведомление `keychain_seal` (1055). Наклейка и слэб расходуются; слэб продаётся в магазине. Снятый с оружия слэб возвращается с той же наклейкой.
- **C: сувенирное качество.** Оружие из набора, у которого есть `tournament event id` (137), становится сувениром: качество 12, атрибуты события и двух команд (137, 139, 140) и золотые наклейки — логотип организатора (`tournament_team_id` 0), обе команды и автограф игрока одной из них (`sticker_kits` с `tournament_event_id` и `_gold` в имени), плюс брелок с моментом матча, как было. Команды случайные: наборы из магазина о матче ничего не знают. Стадия турнира (138) не ставится.
- **C: турнирные товары.** В магазине Austin 2025 и Budapest 2025 (пропуск, пропуск + 3 жетона, жетон, капсулы наклеек и автографов, сувенирные наборы всех 7 карт) и Cologne 2026 (пропуск, пропуск + жетоны, капсулы организаторов и игроков), цены как у Antwerp 2022. Пропуск зрителя (`fan_token`) активируется в монету турнира (`tournament_journal_<event>`), пропуск + жетоны даёт ещё 3 жетона, жетон меняется на случайный сувенирный набор турнира; уведомление `ticket_activated` (9178). Не сделаны: улучшение монеты (Pick'Em) и капсулы Cologne 2026 с плавающей ценой (`volatile container` 3, `VolatileShopSubscribe`).
- **C: граффити.** Подписывать есть чем, патчить нечего: в текущих `client.dll` и `server.dll` нет ни старого ключа CS:GO, ни другого RSA-ключа на 1024 бита, и они не импортируют проверку RSA (она есть только в `filesystem_stdio.dll`, для VPK). Сообщение «Failed to patch graffiti public key… Graffiti will not work» заменено на понятное, подпись каждого спрея пишется в лог (`Graffiti: signed spray`), а чтение полей сообщения больше не выходит за границы, если клиент прислал меньше трёх координат. Появляется ли спрей, покажет запуск.
- **D: шум в логе.** `MatchmakingStart/Stop` молча игнорируются, атрибуты типа `vector` (прицелы AUG/SG 553) — тоже.
- **Исправлено: бесконечная активация хранилища** (проверено в игре). CS2 шлёт имя хранилища (и нейм-теги, и снятие имени) под старыми id, но в protobuf `CMsgModifyItemStringAttr`, а GC понимал только старый struct-формат и писал `unhandled protobuf message k_EMsgGCNameItem`, поэтому окно ждало ответа вечно. Формат взят из `client.dll` (`CProtoBufMsg<CMsgModifyItemStringAttr>` при отправке 1006/1019/1030).
- **D: выделенный сервер на Windows.** `-dedicated` теперь работает без запущенного Steam: `steamclient64.dll` берётся из `game\bin\win64` или рядом с `cs2_gc.exe` (копия из steamcmd).

### D — в последнюю очередь
| Что | Почему | Оценка |
|---|---|---|
| Запуск `cs2_gc.exe` не из папки игры | Код посмотрен: рабочая папка ставится в `<cs2>\game`, чтобы движок нашёл `csgo\gameinfo.gi`; нужен один запуск. `run_cs2_gc.cmd` и так копирует exe к `cs2.exe` | 1 запуск |
| Выделенный сервер: проверка и Linux | Windows-часть готова, но не запускалась; для Linux нет платформенного слоя (`platform_windows.cpp` — единственный) и проверить его здесь негде | ~1–2 часа на Linux |

Плюс поддержка: крупный апдейт CS2 может поменять протобафы или интерфейсы Steam. Обычно это ~15–60 мин: self-test, `tools/dump_protos.py`, пересборка каталога редактора. После апдейта с новыми кейсами списки ножей для `items_extra.txt` печатает `tools/gen_unusual_lists.py` (данные ByMykel/CSGO-API `crates.json`).

## Структура

```
cs2_gc/        GC: логика клиента/сервера, инвентарь, схема предметов, Steam-хуки, VPK
launcher/      cs2_gc.exe
protobufs/     GC-схемы CS2 (дамп из client.dll); csgo_original/ — схемы csgo_gc для сравнения
steamworks/    заголовки Steamworks + steam_old/*023.h + сгенерированные proxy/*023.h
tests/         офлайн self-test (cs2_gc_selftest.exe, --serve для проверки редактора без игры)
tools/         dump_protos, vpk_extract, steamproxygen, vtable_from_flat, gen_message_names, peinfo, gen_invedit_catalog, gen_unusual_lists, make_demo_mod, make_music_mod (+ music_cutter.html), make_sound_mod, make_bot_mod, convert_audio.ps1
editor/        веб-редактор инвентаря (форк csgo_invedit) + каталог CS2
run_cs2_gc.cmd, cleanup_cs2_folder.cmd, open_editor.cmd
examples/      config.txt, inventory.txt, items_extra.txt, price_sheet.txt
reference/     README оригинального csgo_gc
```

## Лицензия

[2-Clause BSD](LICENSE), как у csgo_gc. Copyright (c) Mikko Kokko и соавторы csgo_gc; изменения для CS2 под той же лицензией.
