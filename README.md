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

`blocked_text.txt` / `blocked_radio.txt` — **важное изменение смысла**: в
старом плагине это были списки ключей НАТИВНЫХ сообщений Valve (кэш-award
спам, служебные "Cstrike_TitlesTXT_..."), а не то, что реально пишут игроки.
BetterChat (v1) блокирует по этим файлам **реальный текст, который игрок
печатает в чат** — что на практике полезнее (например, против рекламных
ботов вроде найденных 26.08.2026 "cs2commends.com"/"jaegerservice.xyz").
Подавление нативного спама Valve пока не портировано — отдельная задача,
нужен тест на живом сервере (перехват `IGameEventSystem::PostEventAbstract`
по ключу сообщения).

`blocked_radio.txt` пока пустой — точный формат серверной команды при выборе
конкретной радио-фразы не проверен на живом сервере, доработается после
теста.

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

Готовую сборку берите из вкладки **Actions** этого репозитория (артефакт
`BetterChat-linux` или `BetterChat-so`).

## Что ещё не сделано (v1)

- Подавление нативного спама Valve (кэш-award тексты, "Fire in the hole" и
  т.п. в чате) — было у старого плагина через `blocked_text.txt`/
  `blocked_radio.txt`, здесь не портировано.
- Блокировка радио-фраз (`blocked_radio.txt`) — формат команды не проверен
  на живом сервере.
- Всё остальное (коннект/дисконнект/смена команды без дублей + текстовый
  фильтр чата) реализовано и должно работать сразу после сборки.

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
