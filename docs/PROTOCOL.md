<!--
  Спільний контракт двох прошивок. Копія слово в слово живе в репозиторії супутника
  (modesp-bt-satellite/docs/PROTOCOL.md), і це навмисно: жодна зі сторін не має права
  залежати від того, що сусідня тека взагалі існує на диску.

  Правити тут і там одночасно, і піднімати proto_minor. Розбіжність між копіями — це
  зламаний лінк, а не дрібниця форматування.

  Реалізація P4-боку: components/sat_link, тести tests/host/test_sat_link.cpp.
-->

# Протокол лінка, байт за байтом

Доповнює `DEVICE_SPEC.md`. Тут розкладки, яких там бракувало.

Ця версія **1.7** і вона узгоджена з другою реалізацією. Міняти можна, але правкою тут і
підняттям `proto_minor`.

- **1.7** - host-requested cover retry (`COVER_RETRY`), device capability bit 6.
- **1.6** - bounded unknown-length cover streaming with length/CRC completion.
- **1.5** — track-scoped JPEG covers (RESET/DATA); asynchronous transfer and decoding.
- **1.4** — LHDC V5 codec ID 5; host capability bit 5 authorizes 96 kHz.
- **1.3** — optional acknowledged format transitions (`STREAM_PREPARE` / `STREAM_READY`),
  host capability bit 4; native decoder slot widths for capable hosts. Legacy layouts unchanged.
- **1.2** — `STREAM_FMT` обов'язковий перед **будь-якою** зміною формату, не лише на початку
  з'єднання. Хост має право мовчати, поки формат не оголошений. Розділ «Формат» переписано:
  «усередині з'єднання частота не міняється» було обіцянкою, яку скасував перший же випадок,
  нею не передбачений. У розкладці `HELLO` прибрано `proto_minor = 0`, що лишався з 1.0.
- **1.1** — `caps` хоста, біт 3: приймає 32-бітні слоти.
- **1.0** — початкова.

---

## Кадр

```
0x00 | COBS( type:u8 | seq:u8 | payload… | crc16:u16 ) | 0x00
```

- **Максимум 1040 байтів у декодованому кадрі**, тобто payload до 1036.
- **Приймальний буфер на дроті: 1048 байтів.** Це не те саме число, і обидві реалізації
  мусять заводити саме друге. COBS у найгіршому випадку додає `ceil(n / 254)` байтів
  оверхеду: 1040 + 5 = 1045, плюс два роздільники = 1047. 1048 округлює вгору.
- Провідний `0x00` необов'язковий: кадри впритул ділять один роздільник. Приймач ігнорує
  порожні кадри між роздільниками.
- Приймач, у якого буфер менший, мовчки відкидає завеликий кадр і **не втрачає
  синхронізацію**: наступний `0x00` ставить його на місце. Саме для цього тут COBS.
- `crc16` — CRC-16/CCITT-FALSE, поліном `0x1021`, початкове `0xFFFF`, без фінального XOR,
  над `type | seq | payload` до кодування.
- Числа **little-endian**. Рядок — `u16 len` плюс UTF-8, без завершального нуля,
  порожній рядок це `len = 0`.
- `seq` росте по колу окремо в кожному напрямку.

---

## Повідомлення

D = пристрій, H = хост.

### `0x01` HELLO, обидва напрямки

```
u8   proto_major = 1
u8   proto_minor      // версія цього документа, якої дотримується відправник (див. заголовок)
u32  caps
str  build            // "sat 0.1.0 a1b2c3d"
```

Шлеться першим після старту і у відповідь на чужий HELLO. Далі працюємо за меншим
`proto_minor` з двох.

**Різний `proto_major` означає несумісність.** Лінк не піднімається, обидві сторони пишуть
помилку в `LOG` і більше нічого не шлють, крім `PING`/`PONG` і `LOG`. Аудіо це не зупиняє:
I2S від лінка не залежить, і пристрій, який грає без екрана, кращий за пристрій, який
мовчить.

`caps` пристрою:

| біт | значення |
|---|---|
| 0 | A2DP sink |
| 1 | метадані AVRCP |
| 2 | абсолютна гучність AVRCP |
| 3 | обкладинка |
| 4 | розширювач портів |
| 5 | АЦП на лініях розширювача |
| 6 | honours `COVER_RETRY` (`proto_minor` >= 7; requires bit 3) |

`caps` хоста:

| біт | значення |
|---|---|
| 0 | **уміє перелочити I2S на льоту** за `STREAM_FMT` |
| 1 | хоче обкладинку |
| 2 | керує розширювачем |
| 3 | **приймає 32-бітні слоти** (`proto_minor` >= 1) |
| 4 | FORMAT_READY: acknowledges prepared formats (`proto_minor` >= 3) |
| 5 | accepts 96 kHz in 16/32-bit stereo slots (`proto_minor` >= 4; requires bits 0 and 4) |
| 6 | accepts streaming covers with END (`proto_minor` >= 6; requires bit 1) |

**Bit 5 is an explicit frequency capability.** RELOCK alone authorizes only
44.1/48 kHz. Offer LHDC V5 at 96 kHz only after HELLO 1.4 with bits 0, 4 and 5;
missing, older or incomplete HELLO keeps the endpoint at 44.1/48 kHz.
96 kHz transitions use STREAM_PREPARE/READY and the existing 50 ms guard.
A 96 kHz stream must remain gated if a replacement host lacks this capability.
LHDC V5 carries native 16-bit PCM or 24 significant bits in 32-bit containers.
The endpoint does not offer 192 kHz, LL, spatial processing or lossless modes.

**Біт 3 додано у версії 1.1.** Він каже, що `STREAM_FMT.bits = 32` хост переживе: вхідний
канал переналаштовується під оголошену ширину, а не читає 32-бітний потік конфігурацією на 16.
Без цього біта пристрій мусить лишатись на 16 бітах і дизерити, бо помилка тут мовчазна —
половина кадрів і два канали, склеєні в одне слово.

Біт сам собою і є узгодженням: хост версії 1.0 його не виставить ніколи. Але пристрій має
дивитись саме на біт, а не на `proto_minor`, бо хост може вміти 1.1 і все одно не приймати
широкі слоти.

**Чого це коштує хосту, щоб не було сюрпризу.** 32-бітні слоти на 44,1 і 48 кГц вимагають від
приймача внутрішнього такту, якого кварц не дає (потрібно 22,6 і 24,6 МГц при джерелі вдвічі
вищому, а кварц 40 МГц). Тому на час такого потоку вхідний канал переходить на APLL, і поки
він грає, **вихідний такт хоста прив'язаний до сім'ї цього потоку**. Для слухача це нічого не
змінює — Bluetooth і є джерелом у ці хвилини, — але 96 кГц і 44,1 в один момент неможливі.

### `0x02` PING, обидва

```
u32  tick_ms          // час роботи відправника
```

### `0x03` PONG, обидва

```
u32  echo_ms          // tick_ms з PING
u32  own_ms           // час роботи того, хто відповідає
```

### `0x10` BT_STATE, D → H

```
u8   state            // 0 off, 1 idle, 2 discoverable, 3 connecting, 4 connected, 5 streaming
u8   codec            // 0 none, 1 SBC, 2 AAC, 3 aptX, 4 aptX HD, 5 LHDC V5
u8   addr[6]          // BD_ADDR партнера, нулі коли не підключено
str  name             // ім'я партнера, порожнє коли невідоме
```

За подією.

### `0x11` TRACK_META, D → H

```
str  title
str  artist
str  album
```

На зміну треку. Порожній рядок = невідомо.

**Перший `TRACK_META` після підключення майже напевно порожній**, і це нормальна поведінка, а
не помилка. На стенді 18.09.2026 перший запит метаданих одразу після з'єднання повернувся з
порожніми полями, а наповнила їх нотифікація про зміну треку приблизно через десять секунд.

Отже: пристрій шле `TRACK_META` щойно має що слати, навіть порожній, і шле **ще раз**, коли
поля наповнились. Хост тримає це як «останнє відоме» і не вважає перше повідомлення
остаточним.

### `0x12` TRACK_POS, D → H

```
u8   state            // 0 stopped, 1 playing, 2 paused, 3 fwd seek, 4 rev seek
u32  elapsed_ms
u32  total_ms         // 0 = невідомо
```

Раз на секунду **під час програвання**, і негайно на зміну стану. У паузі й у стопі не
шлеться взагалі.

Звідки береться позиція, залежить від телефона, і пристрій має пробувати дешевший шлях
першим:

1. Якщо в масці нотифікацій партнера є `PLAY_POS`, зареєструватись на неї і слати `TRACK_POS`
   на кожну подію, плюс раз на секунду для рівного руху смужки.
2. Якщо `PLAY_POS` у масці **немає** — а на стенді 18.09.2026 телефон оголосив маску `0x1f26`,
   де біта `0x20` немає — тягнути `GetPlayStatus` опитуванням раз на секунду. Один AVRCP-запит
   на секунду по повітрю це прийнятно; частіше не треба, смужка на екрані все одно рухається
   з кроком у секунду.

**`total_ms` завжди санітизувати.** Той самий телефон повернув довжину `−1000`. Усе, що
від'ємне або більше за добу, перетворювати на `0`, тобто «невідомо».

### `0x13` STREAM_FMT, D → H

Legacy path: used when FORMAT_READY is not negotiated. For a capable 1.3 host,
`STREAM_PREPARE` replaces this announcement; its acknowledged rules take precedence.

```
u32  rate_hz
u8   bits             // ширина слота на дроті: 16 або 32
u8   channels         // 2
```

**До першого семпла кожного з'єднання A2DP — і перед будь-якою іншою зміною формату на
дроті.**

Версія 1.0 називала лише початок з'єднання, і цього виявилось замало. 19.09.2026 пристрій
змінив ширину слота посеред уже піднятого з'єднання — у відповідь на біт 3 у `caps` хоста, а
не на нове підключення телефона, — і тригера для оголошення не було. Хост читав 32-бітний
потік конфігурацією на 16, і з колонок пішов шум.

Тому правило формулюється не через подію, а через сам факт:

- **пристрій не міняє частоту або ширину, не оголосивши їх наперед.** Причина зміни значення
  не має: нове з'єднання A2DP, інший кодек, зміна `caps` хоста, перезапуск власного I2S;
- після оголошення — не менш ніж **50 мс тиші** на I2S, і лише потім семпли нового формату;
- `STREAM_FMT` повторюється у відповідь на `HELLO`, бо хост міг перезапуститись і пропустити
  оголошення;
- формат забувається на роз'єднання: оголошувати застарілий гірше, ніж не оголошувати.

**Хост має право не грати, поки формат не оголошений**, і це не помилка, а єдина безпечна
поведінка: вгадана ширина звучить як шум, а не як тиша, і на слух її не відрізнити від
поганого кодека.

### `0x14` COVER, D → H

```
u16  chunk            // з нуля
u16  total            // скільки шматків усього
u8   mime             // 0 невідомо, 1 jpeg, 2 png
u32  total_len        // повний розмір зображення
u16  len              // байтів у цьому шматку, максимум 1024
u8   data[len]
```

`mime` і `total_len` повторюються в кожному шматку навмисно: сім байтів надлишку на
кілобайт роблять кожен шматок самоописним, і втрата першого не ламає збірку.

**Стеля: 256 КБ, тобто 256 шматків.** Поля `u16` дозволяють 64 МБ, але буфер треба рахувати
з чогось. Якщо зображення більше за стелю, пристрій **не шле його взагалі** і пише про це в
`LOG`. Обрізана картинка гірша за відсутню: хост не має способу відрізнити її від втраченого
хвоста.

### `0x15` VOLUME_STATE, D → H

```
u8   volume           // 0..127
u8   source           // 0 змінено локально, 1 змінено партнером
```

На кожну зміну. Це те, чого бракувало: коли гучність крутить телефон, абсолютна гучність
AVRCP приходить сюди, і хост має про це дізнатись.

### `0x16` STREAM_PREPARE, D → H (1.3)

```
u32  boot_nonce       // nonzero random identity for this satellite boot
u32  generation       // nonzero, incremented for every transition/reconnect
u32  rate_hz
u8   bits             // I2S slot width: 16 or 32
u8   channels         // 2
```

Exactly 14 payload bytes, little-endian. Used only if host HELLO has minor >= 3
AND host capability bit 4. Bit 3 remains independent permission to use 32-bit slots.
For such hosts, SBC/AAC use native 16-bit slots; the current aptX/aptX HD decoders
use 32-bit containers for their 24 significant bits. Without bit 3, keep narrowing
with dither. Legacy hosts keep their existing 16/always-32 capability policy.

Before announcing, close the PCM producer gate, finish any in-flight writer, and
clear application buffers, carry and TX DMA. Transmit the announcement before
changing I2S format. Keep I2S data silent until successful local configuration,
matching READY success AND at least 50 ms from confirmed UART transmission.
Enqueueing a frame does not start the timer. No timeout substitutes for READY.
Retry the same PREPARE every 250 ms while waiting; queues remain bounded.

The host publishes the whole transaction immediately, independently of telemetry.
With BT selected, fade buffered old audio, pause RX, await its last publication,
discard old buffers and reset ASRC history, apply the supported clock/slot plan,
and start RX before reporting success. If another source is selected, save the
request and defer preparation/READY until BT selection. Leaving BT invalidates
readiness; selecting it again requires preparation even for the same request.

Exact duplicate IDs/tuples are idempotent; the same ID with a different tuple,
or an older generation within the current boot, is refused. Disconnect clears
format state. Reconnect, including the same rate/width, gets a new generation.
On generation wrap the satellite generates a fresh nonzero boot nonce.
HELLO triggers reannouncement so a restarted host can learn the current format.

### `0x20` CMD_TRANSPORT, H → D

```
u8   cmd              // 0 play, 1 pause, 2 stop, 3 next, 4 prev,
                      // 5 ffwd start, 6 ffwd stop, 7 rew start, 8 rew stop
```

### `0x21` CMD_VOLUME, H → D

```
u8   volume           // 0..127
```

### `0x22` CMD_RADIO, H → D

```
u8   radio            // 0 вимкнути, 1 увімкнути
u8   discoverable     // 0 ні, 1 так
u8   disconnect       // 1 = розірвати поточне з'єднання
```

### `0x23` STREAM_READY, H → D (1.3)

```
u32  boot_nonce       // copied from STREAM_PREPARE
u32  generation       // copied from STREAM_PREPARE
u8   status           // 0 prepared; 1 refused
```

Exactly 9 bytes. Unknown status, wrong length and zero identities are invalid.
Only matching nonce AND generation can release the current stream. A refusal is
terminal for that transaction and keeps PCM gated; a later success for the same
ID cannot undo it. Old/duplicate READY frames cannot release a newer transition.
The host repeats the current result at 100 ms intervals to recover a lost reply.
I2S configuration failures remain silent; local satellite driver failures require
restart before another attempt. An established stream survives missing liveness
messages, but a pending acknowledged transition never falls back to timer-only.

### `0x24` COVER_RETRY, H → D (1.7)

```
u32  boot_nonce       // the pair whose transfer failed
u32  track_generation
```

Exactly 8 bytes, little endian, both nonzero. Sent only to a device whose HELLO
carries minor >= 7 and device capability bit 6; otherwise never sent at all, and
the 1.6 behaviour stands unchanged. A device that does not implement it ignores
an unknown type, so a mismatched pair costs nothing either way.

The host sends it when a transfer for that pair ended without a usable image: a
gap, a duplicate or a malformed part, a length or CRC mismatch at `COVER_END`, or
the five-second silence timeout. It is a hint, not a request-response pair — there
is no reply, no NACK and no timeout on it, and a host that never hears back simply
keeps the empty tile 1.6 would have left.

The device honours it only when the pair names its current boot nonce and its
current track generation. Anything else is ignored: the phone has moved on, and
the next `COVER_RESET` carries the new image anyway.

**Honouring it means a NEW generation.** The device repeats the OBEX GET and sends
`COVER_RESET` with a fresh `track_generation`, then `COVER_DATA` from offset zero
and `COVER_END` as usual. Reusing the failed generation would be wrong, not merely
inelegant: `COVER_RESET` with a repeated pair is defined above as idempotent, so
the host would clear nothing and the retry would arrive against a failed assembly.

Both sides bound it, and neither depends on the other's bound. The host asks at
most twice for one image, its allowance restarting when an image completes or when
a `COVER_RESET` arrives that was not the answer to its own asking; the device
serves at most twice per track, counted from the last AVRCP track change or the
last transfer that completed. Whichever limit is reached first ends the chain and the
tile stays empty. This exists because the observed fault is one dropped frame in a
50 KiB image (bench 21.09.2026: `received 18036, next offset 18537`), and one
repeat is enough for that; a phone or a link that loses frames steadily is a
different fault and must not be answered with unbounded OBEX traffic.

### `0x30` GPIO_IN, D → H

```
u32  values           // біт на індекс, 0..31
u32  changed          // які біти змінились від попереднього GPIO_IN
```

За подією, після антидребезгу. Не за опитуванням.

**Одразу після обробки `GPIO_CFG` пристрій шле один `GPIO_IN`**, у якому `changed` = усі
лінії, налаштовані входами цим повідомленням. Інакше кнопка, якої ніхто не торкався, для
хоста не існує, і початковий стан входів він не дізнається ніколи.

### `0x31` GPIO_OUT, H → D

```
u32  values
u32  mask             // застосовуються лише біти, виставлені в mask
```

### `0x32` GPIO_CFG, H → D

```
u8   count
count разів:
  u8 index
  u8 mode             // 0 вимкнено, 1 вхід, 2 вхід pull-up, 3 вхід pull-down,
                      // 4 вихід, 5 вихід open-drain, 6 аналоговий вхід
  u8 initial          // виходи: початковий рівень
                      // режим 6: період віддачі в сотнях мс, 0 = тільки на запит
```

### `0x33` GPIO_MAP, D → H

```
u8   count            // скільки ліній пристрій віддає
count разів:
  u8 index            // як їх адресує хост
  u8 gpio             // номер GPIO ESP32, інформативно
  u8 flags            // див. таблицю нижче
```

Шлеться одразу після HELLO, **завжди**, навіть коли `count = 0`. Стартова конфігурація —
рівно цей випадок.

`caps` біт 4 означає «пристрій розуміє повідомлення розширювача», а не «має хоч одну ногу».
Він виставлений і при `count = 0`. Кількість ліній каже `GPIO_MAP`, можливість — біт.

`flags` на лінію:

| біт | значення |
|---|---|
| 0 | вміє вхід |
| 1 | вміє вихід |
| 2 | вміє АЦП |
| 3 | **має внутрішню підтяжку** |

Біт 3 існує тому, що на ESP32 ноги 34-39 лише входи й **вбудованих підтяжок не мають**
(`driver/gpio.h`: «Input-only GPIOs 34-39 do not have integrated pull-up and pull-down
resistors»), а вони в пулі розширювача. Хост зобов'язаний дивитись на цей біт перед тим, як
просити режим 2 або 3.

Якщо все ж попросив: пристрій ставить найближчий законний режим (звичайний вхід) і пише
попередження в `LOG`. NACK для GPIO_CFG у протоколі немає навмисно — тут немає пар запит-відповідь, і
заводити їх заради одного випадку непропорційно.

### `0x34` ADC_VALUE, D → H

```
u8   index
u16  raw              // сирий відлік АЦП
u16  mv               // мілівольти, скалібровані на пристрої
```

Етап 5, не раніше. Існує тому, що `caps` біт 5 і `GPIO_MAP` flags біт 2 обіцяють АЦП, а
`GPIO_IN` це дві бітові маски й аналогове значення в нього не вкладається. Обіцянка без
повідомлення була б брехнею.

### `0x35` ADC_READ, H → D

```
u8   index
```

Одноразовий запит. Періодичну віддачу вмикає `GPIO_CFG` режимом 6 (аналоговий вхід), де поле
`initial` несе період у сотнях мілісекунд, а `0` означає «тільки на запит».

### `0x40` LOG, D → H

```
u8   level            // 0 error, 1 warn, 2 info, 3 debug
str  text
```

---

## Адресація ніг: індексом, не номером GPIO

Хост адресує лінії **індексом 0..N-1**. Номер GPIO ESP32 він бачить у `GPIO_MAP`, але лише
щоб показати людині в логах.

Карта індекс → нога живе в прошивці цього пристрою, бо це **його власна** плата. Що на тій
нозі висить і як воно називається, вирішує хост через `GPIO_CFG`. Так зміна чужої плати не
вимагає перепрошивки цього пристрою, а зміна цієї плати не вимагає правок у хоста.

Максимум 32 лінії, бо `GPIO_IN` і `GPIO_OUT` носять маски в `u32`.

---

## Формат: зазвичай один на з'єднання, але це не гарантія

For a negotiated 1.3 peer, use the acknowledged 0x16/0x23 transition above.
The STREAM_FMT + 50 ms rule below describes compatibility with legacy peers.

Версія 1.0 казала тут «усередині з'єднання частота не міняється», і 1.2 це скасувала. Правило
було написане як обіцянка, а виявилось описом звичайного випадку: 19.09.2026 пристрій змінив
**ширину слота** посеред уже піднятого з'єднання — у відповідь на біт 3 у `caps` хоста, а не
на нове підключення телефона. Обіцянка не витримала першого ж не передбаченого нею приводу.

Тому формулювання таке:

- телефон узгоджує частоту на початку з'єднання, і **зазвичай** вона тримається до кінця;
- але жодна зі сторін не має права на це **розраховувати**. Формат може змінитись, і причини
  не вичерпуються новим з'єднанням: інший кодек, зміна `caps` хоста, перезапуск I2S пристрою;
- будь-яка зміна — частоти чи ширини — **оголошується наперед** через `STREAM_FMT`, далі не
  менш ніж 50 мс тиші на I2S, і лише потім семпли нового формату.

Якщо хост у HELLO **не** виставив біт 0 (уміє перелочитись), пристрій анонсує в A2DP **тільки
44,1 кГц**, щоб телефон не мав з чого вибирати. Це простіше й чесніше, ніж міняти частоту
під хостом, який цього не переживе.


### Track-scoped JPEG covers (protocol 1.5)

Host capability bit 1 enables these messages only with host minor >= 5.
New hosts do not consume legacy COVER 0x14; pre-1.5 hosts receive no cover
from this implementation. Audio/metadata remain compatible. Existing layouts
and framing sizes are unchanged.

`0x17 COVER_RESET`, device to host, critical queue: `u32 boot_nonce`,
`u32 track_generation`. Both nonzero. A new pair clears the displayed Bluetooth
cover and cancels assembly/decode of the old image. A repeated pair is idempotent.
Sent on AVRCP connection/track change and host refresh, before fetching the image.
No image/unsupported phone means the tile remains empty. File covers are separate.

`0x18 COVER_DATA`, device to host, droppable queue: `u32 boot_nonce`,
`u32 track_generation`, `u32 total_bytes`, `u32 offset`, followed by 1..1000
JPEG bytes. All integers little endian. There is no inner length: frame payload
length minus 16 gives the byte count. Every non-final part has 1000 bytes.
Host requires the current RESET pair, offset zero first, contiguous offsets,
identical total size and an exact final length. Unknown/old pairs are ignored.
A gap/duplicate/malformed current part aborts assembly; a missing tail times out
after 5 seconds. Never publish a partial image. Hard protocol bound 256 KiB;
the WROOM sender only requests linked thumbnails.
Maximum new payload is 1016 bytes, within the existing 1036-byte limit.

Sender note (satellite, 2026-09-21): the satellite no longer implements the
known-length collection path. It sends cover messages only to a host that
announces minor >= 6 with bits 1 and 6 (streaming, below); a host below that
receives no RESET, DATA or legacy 0x14 COVER at all. This is a floor on the
sender side, not a change to any layout or number.

RESET has priority over queued old DATA; the pair prevents cross-track mixing.
Bluetooth callbacks never wait for UART draining. A separate low-priority task
paces DATA and abandons it if the epoch changes or the queue stalls for 2 s.
The P4 receives into PSRAM; JPEG decoding is off the UART/module task, and
publication checks the current generation again after decoding.


### Bounded streaming JPEG covers (protocol 1.6)

Host capability bit 6 (COVER_STREAM_CAP) requires host minor >=6 AND cover bit 1.
The satellite sends no covers to a 1.5 host (see the sender note above); a 1.6 host
continues accepting known-length 1.5 DATA. Audio formats and capabilities do not change.

With streaming negotiated, DATA 0x18 uses total_bytes=0 (unknown until OBEX final).
Each DATA carries 1..1000 bytes, contiguous offsets starting at zero. Part sizes
may vary. The host reserves at most 256 KiB in PSRAM and never publishes on DATA.
Mixing known/unknown totals within one generation is invalid. RESET is unchanged.

`0x19 COVER_END`, device to host: `u32 boot_nonce`, `u32 track_generation`,
`u32 actual_bytes`, `u32 crc32`. Exactly 16 bytes, all little endian.
CRC-32/ISO-HDLC covers the concatenated JPEG bytes (check `123456789` = CB F4 39 26
as numeric 0xCBF43926, wire bytes 26 39 F4 CB; polynomial 0xEDB88320 reflected, initial/final xor 0xFFFFFFFF).
END uses the SAME bulk FIFO as DATA and cannot overtake it. It is emitted only
after successful OBEX final. Host commits only if current pair, length and CRC
all match, length is 3..262144, and no gap/duplicate/error has occurred.
Missing END/parts time out after 5 seconds without received data. Old epochs
remain ignored. No partial/corrupt image is published; no automatic retransmission.

ESP32 copies borrowed OBEX callback bytes into bounded UART queue allocations,
never retains the entire JPEG, never waits on the callback task, never evicts
another frame for cover streaming, and retains the 16-KiB heap reserve. Queue
pressure, invalid JPEG or the 256-KiB limit cancels this image without sending END.
The existing 512-byte OBEX MTU / 50-ms continuation pacing remains. Packet total
size need not be supplied by the phone. UART TX still waits on the hardware FIFO
semaphore, preserving the audio decoder's opportunity to run.


### Cover retry (protocol 1.7)

Until 1.7 a cover lost one frame and was gone. The 1.6 rules are deliberately
strict — a gap aborts assembly, `COVER_END` commits only on an exact length and
CRC, and a partial image is never published — and they are right, because half a
picture is worse than none. What was missing was a way to ask again.

`COVER_RETRY` 0x24 is that, and it is one message and one capability bit. Device
capability bit 6 says the device honours it; it requires device bit 3 and device
minor >= 7. No host capability bit is added, because there is nothing the device
would do differently for knowing: the image handle and the OBEX session are kept
between tracks already, so serving a retry needs no state held in advance.

**The panel and the satellite are flashed separately, so the mismatched pairs are
the normal case and both are defined:**

- **1.7 host, pre-1.7 device.** A 1.6 device never sets bit 6, so the host never
  sends 0x24 and the link behaves exactly as 1.6. Were one sent anyway, the
  device's dispatcher counts it as an unknown type and ignores it — no error, no
  desync, and nothing waiting for a reply.
- **Pre-1.7 host, 1.7 device.** The host never sends 0x24, so the device never
  retries. Bit 6 is advertised and unused. Again exactly 1.6.
- **Both 1.7.** A failed transfer costs one repeat of the OBEX GET under a new
  generation, bounded on both sides as the message describes.

Neither side may treat the absence of a retry as a fault, and neither may wait for
one: a cover that never arrives leaves the tile empty, which is what happens today
and is a correct outcome, not a degraded one.
