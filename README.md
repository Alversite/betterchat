# BetterChat (CS2)

Metamod:Source плагин для Counter-Strike 2 — замена старого `chat_cleaner`:
кастомные сообщения о подключении / отключении / смене команды + фильтр
чата. Переписан с нуля под актуальный `hl2sdk-cs2` + Metamod:Source,
собирается только под **Linux** (`linuxsteamrt64`) — под Windows не собирается
и не нужен.

## Почему переписан, а не починен старый

У `chat_cleaner` сообщение о подключении дублировалось для каждого игрока.
Причина — движок CS2 у каждого реального клиента дважды инициирует
подключение на сетевом уровне: первая попытка обрывается с
`NETWORK_DISCONNECT_LOOPSHUTDOWN`, тут же следует вторая, которая и
остаётся. `chat_cleaner` вешал своё сообщение на событие, которое стреляет
на КАЖДОЙ из этих попыток. BetterChat вместо этого хукает
`ISource2GameClients::ClientPutInServer` — оно стреляет ровно один раз,
когда игрок реально и окончательно зашёл в игру, поэтому дублировать
физически нечему.

## Конфиг

Формат `settings.ini` — **тот же самый** (Valve KeyValues), что у старого
`chat_cleaner`: `DebugMode`, `CustomTeamMessages`, `CustomConnectMessages`,
`CustomDisconnectMessages`. Добавлен один новый необязательный ключ:
`ConnectDedupSeconds` (доп. защита от повторного сообщения, на случай если
движок когда-нибудь снова начнёт дублировать событие — сейчас это просто
подстраховка, не более).

`blocked_text.txt` / `blocked_radio.txt` — **тот же смысл, что в старом
плагине**: списки ключей НАТИВНЫХ сообщений Valve (кэш-award спам,
служебные "Cstrike_TitlesTXT_Game_connected", радио-реплики типа "Fire in
the hole" и т.п.), которые подавляются, чтобы вместо них показывался только
кастомный текст BetterChat. Списки перенесены без изменений.

Технически это работает через перехват `IGameEventSystem::PostEventAbstract`
(тот оверлоад, через который движок шлёт СВОИ сообщения — другой слот
виртуальной таблицы, не тот, что использует сам BetterChat для отправки
своих сообщений, так что самого себя плагин никогда не блокирует). Ищем
`UM_TextMsg`, сверяем `param(0)` со списком, если совпало — обнуляем список
получателей у уже готового к отправке сообщения (тот же приём, что в
`cs2kz-metamod`, публичном проде-плагине для CS2).

`blocked_chat_words.txt` — **новое, этого не было в chat_cleaner**: список
подстрок в РЕАЛЬНОМ тексте, который печатают игроки (`say`/`say_team`),
блокируется до того, как сообщение вообще попадёт в чат. Стартовый список —
рекламные боты, пойманные в логах 26.08.2026
("cs2commends.com"/"jaegerservice.xyz").

## Установка

1. Metamod:Source для CS2 уже должен быть установлен.
2. Скопируйте на сервер (в `game/csgo/`):
   - `addons/BetterChat/BetterChat.so`
   - `addons/metamod/BetterChat.vdf`
   - `addons/configs/BetterChat/` (settings.ini, blocked_text.txt, blocked_radio.txt, blocked_events.txt)
   - `addons/translations/betterchat.phrases.txt` (справочно, реальный текст захардкожен в плагине)
3. **Уберите старый `chat_cleaner`** (`addons/chat_cleaner/`, `addons/metamod/chat_cleaner.vdf`,
   `addons/configs/chat_cleaner/`, `addons/translations/chat_cleaner.phrases.txt`) — иначе
   получите два одновременно работающих плагина с частично одинаковой функцией.
4. Перезапустите сервер (или `meta load addons/BetterChat/BetterChat`).

Готовую сборку берите со страницы **Releases** этого репозитория —
`BetterChat-linux.zip`, один файл, распаковывается прямо в `game/csgo/`.

## Статус

Собирается и линкуется чисто (CI), но **живьём на сервере ещё не
проверялось** — SourceHook на виртуальные методы может скомпилироваться без
ошибок и всё равно не сработать в рантайме, если где-то ошибся с сигнатурой.
Особенно стоит проверить именно перехват `PostEventAbstract` — это два
разных оверлоада одного и того же виртуального метода, и хотя `cs2kz-metamod`
использует ровно такую же связку в проде, до реального теста на сервере
это не 100% гарантия для конкретно этой сборки SDK.

Всё, что было у старого `chat_cleaner`, перенесено: цветные
коннект/дисконнект/смена-команды сообщения (без бага задвоения), подавление
нативного спама Valve по `blocked_text.txt`/`blocked_radio.txt`. Плюс
добавлен фильтр реального текста игроков (`blocked_chat_words.txt`), которого
раньше не было.

## Сборка

Собирается в CI (GitHub Actions, контейнер SteamRT sniper). Локально (только Linux):

```bash
export HL2SDKCS2=/path/to/hl2sdk-cs2
export MMSOURCE_DEV=/path/to/metamod-source
export HL2SDKMANIFESTS=/path/to/hl2sdk-manifests
mkdir build && cd build
python ../configure.py --enable-optimize --sdks cs2
ambuild
```

Результат: `build/package/addons/BetterChat/BetterChat.so`.
