# Специфікація пристрою

Що треба побудувати. Байтові розкладки повідомлень — у `PROTOCOL.md`, це єдиний інший
документ у цьому проекті. Більше нічого дочитувати не треба.

---

## Пристрій

Плата на ESP32, яка приймає аудіо з телефона по Bluetooth і віддає його **хосту** двома
шляхами: звук по I2S, усе інше по UART. Додатково віддає хосту кілька своїх GPIO.

Хост — це чужий мікроконтролер. Що він таке, цей проект не цікавить.

---

## Залізо

| | |
|---|---|
| Чип | **ESP32-WROOM-32** на платі DevKitC. Рішення замовника 18.09.2026 |
| Чому не S3 / C3 / C6 | тільки в оригінального є Bluetooth Classic, а без нього немає A2DP |
| Живлення | власний регулятор від 0,5 А, 22 мкФ на піні. Піки передачі близько 130 мА |
| Такт | **240 МГц**, не типові 160. Декодер aptX інакше не встигає: на 160 МГц і `-Og` він
  тримав ядро на 100%, губив дві третини пакетів і будив сторожовий таймер |
| Wi-Fi | вимкнений у Kconfig повністю. Пристрій ніколи не виходить у мережу |
| Інтерфейси назовні | I2S (3 лінії), UART (2 лінії), плюс вільні GPIO |
| Порт прошивки й логів | **COM13** (USB-Enhanced-SERIAL CH9102 на платі DevKitC) |

WROOM, а не WROVER, і не тільки через PSRAM, якої тут не треба. На WROVER **GPIO16 і GPIO17
зайняті під PSRAM** і назовні не виходять. На WROOM вони вільні, і саме на них лягає UART.

---

## Bluetooth

- **A2DP sink** і **AVRCP controller**.
- Кодеки: **SBC, AAC, aptX, aptX HD, LHDC V5**. LDAC не переноситься: єдиний декодер, який є в
  мережі, не має ліцензії, а Sony публікує лише кодер.
- Парування, автоперепідключення після ввімкнення, одне активне з'єднання.
- З протоколу 1.8 спарювання підтверджує людина на хості, а налаштування Bluetooth (радіо,
  видимість, автоперепідключення, кодеки, ім'я) і список спарених хост показує й міняє. Див.
  розділ «Налаштування Bluetooth з хоста» нижче.

---

## Вихід I2S

- **Пристрій — майстер.** Він генерує `BCLK` і `WS`, хост їх приймає.
- Лінії: `BCLK`, `WS`, `DOUT`. **MCLK не потрібен і не виводиться.**
- Стерео, 44,1 / 48 кГц; LHDC V5 також 96 кГц за HELLO 1.4 capability bit 5, слоти 16 або 32 біти, формат Philips I2S.
- Частота і розрядність оголошуються по UART **до першого семпла**, а не вгадуються хостом.
- Rate and width may change within an A2DP connection. Announce every transition
  using the negotiated protocol in `PROTOCOL.md`; never assume a lifetime format.
  Without host RELOCK capability, A2DP still advertises only 44.1 kHz.
- With protocol 1.3 FORMAT_READY and WIDE, use native decoder widths: SBC/AAC 16;
  aptX/aptX HD 32-bit containers (24 significant bits); LHDC V5 uses 16-bit
  slots for 16-bit content and 32-bit containers for 24-bit content. WIDE is a capability,
  not an instruction to widen all samples. Legacy width policy remains compatible.
- A negotiated transition stays silent until correlated host readiness and the
  minimum 50 ms after completed UART transmission; lost ACK is retried, not ignored.
- **Кодек може нести більше, ніж пройде в слот.** aptX HD дає 24 біти; у 16-бітних слотах
  вони звужуються з трикутним дизером, а не відкиданням молодшого байта. Відкидання зміщує
  кожен відлік в один бік і лишає помилку, корельовану з сигналом, що на тихому чути як
  призвук. Тридцятидвобітні слоти прибирають це питання разом із звуженням.

Чому майстер: пристрій приймає потік, темп якого задає кварц телефона. Як майстер він керує
власним APLL і може підстроюватись під дрейф. Як раб він не керує нічим.

Підстроювання (етап 10.1, 2026-09-21): раз на секунду середнє заповнення кільця за цю
секунду порівнюється з серединою кільця, і MCLK підлаштовується через
`i2s_channel_tune_rate()` — ПІ-регулятор з похибкою в мілісекундах звуку (однакова динаміка
на 44,1/16 і 96/32), 10 ppm на мілісекунду, інтегратор зі сталою 400 с, авторитет ±200 ppm,
крок не менше 2 ppm. Працює лише під час потоку з відкритим гейтом, завмирає між треками,
скидається при зміні формату; застосований ppm є у звіті раз на 10 с. Два контури
послідовно: цей стежить за телефоном, ASRC хоста (±500 ppm) — за BCLK цього пристрою;
контур хоста має лишатись швидшим. Драйвер переписує коефіцієнти APLL наживо на кожному
кроці — **безперервність BCLK під час кроку перевірити осцилографом на стенді**.

---

## Лінк керування

UART **460800 8N1**, без апаратного керування потоком.

### Кадр

```
0x00 | COBS( type:u8 | seq:u8 | payload… | crc16:u16 ) | 0x00
```

- `0x00` — роздільник; усередині кадру нулів немає за побудовою COBS, тому приймач
  синхронізується з першого ж роздільника після ввімкнення живлення.
- `crc16` — CRC-16/CCITT-FALSE над `type | seq | payload`, до кодування COBS.
- `seq` росте по колу окремо в кожному напрямку. Приймач логує розриви, повторів не вимагає.
- Числа **little-endian**. Рядки — `u16 len` плюс UTF-8 без завершального нуля.

### Повідомлення

D = пристрій, H = хост.

| Код | Напрям | Зміст |
|---|---|---|
| `0x01` HELLO | обидва | версія протоколу, версія збірки, маска можливостей |
| `0x02` PING / `0x03` PONG | обидва | живість, раз на секунду |
| `0x10` BT_STATE | D → H | підключено, ім'я і адреса пристрою, кодек |
| `0x11` TRACK_META | D → H | назва, виконавець, альбом |
| `0x12` TRACK_POS | D → H | стан, минуло мс, всього мс |
| `0x13` STREAM_FMT | D → H | частота, розрядність, канали. **До першого семпла** |
| `0x14` COVER | D → H | обкладинка з AVRCP, шматками до 1024 байтів |
| `0x15` VOLUME_STATE | D → H | гучність змінилась, і ким саме |
| `0x16` STREAM_PREPARE | D → H | transaction identity and next format (1.3) |
| `0x1A` PAIR_REQUEST | D → H | запит на спарювання: адреса, спосіб, код, ім'я телефона (1.8) |
| `0x1B` PAIR_RESULT | D → H | чим закінчилось спарювання (1.8) |
| `0x1C` BT_CONFIG | D → H | збережені налаштування Bluetooth і що є в збірці (1.8) |
| `0x1D` BOND_LIST | D → H | спарені телефони, від останнього підключеного (1.8) |
| `0x20` CMD_TRANSPORT | H → D | play, pause, stop, next, prev |
| `0x21` CMD_VOLUME | H → D | абсолютна гучність AVRCP, 0..127 |
| `0x22` CMD_RADIO | H → D | радіо увімк/вимк, видимість для парування |
| `0x23` STREAM_READY | H → D | matching identity and preparation result (1.3) |
| `0x25` PAIR_REPLY | H → D | так чи ні на поточний запит спарювання (1.8) |
| `0x26` BT_CONFIG_SET | H → D | нові налаштування Bluetooth, повний стан (1.8) |
| `0x27` BOND_REMOVE | H → D | забути один спарений телефон або всі (1.8) |
| `0x28` BT_REFRESH | H → D | прислати `BT_CONFIG` і `BOND_LIST` (1.8) |
| `0x30` GPIO_IN | D → H | стан входів і маска змінених |
| `0x31` GPIO_OUT | H → D | значення і маска виходів |
| `0x32` GPIO_CFG | H → D | напрямок і підтяжка, один раз на старті |
| `0x33` GPIO_MAP | D → H | які лінії пристрій віддає і що вони вміють |
| `0x34` ADC_VALUE | D → H | аналогове значення лінії. Етап 5 |
| `0x35` ADC_READ | H → D | прочитати аналогову лінію раз. Етап 5 |
| `0x40` LOG | D → H | рівень і текст |

---

## Розширювач портів

Пристрій віддає хосту свої вільні GPIO. Одразу після HELLO він шле `GPIO_MAP`: скільки
ліній, який індекс у кожної і що вона вміє. Далі хост адресує їх **індексом**, а не номером
GPIO, і задає напрямки через `GPIO_CFG`.

- Карта індекс → нога живе тут, бо це **власна** плата пристрою. Що на нозі висить і як воно
  називається, вирішує хост. Так зміна чужої плати не вимагає перепрошивки цього пристрою, а
  зміна цієї не вимагає правок у хоста.
- Максимум 32 лінії: маски в `GPIO_IN` і `GPIO_OUT` це `u32`.
- Антидребезг 20 мс — тут, а не в хоста.
- `GPIO_IN` шлеться **за подією**, не за опитуванням. Від фронту до кадру в лінії — менш ніж
  30 мс.
- Не використовувати strapping-піни ESP32 як виходи: GPIO0, 2, 5, 12, 15.

---

## Поведінка

1. **Ніколи не блокуватись.** Якщо хост перестав читати UART, відкидати найстаріші
   `TRACK_POS`, `LOG` і `COVER`. `GPIO_IN`, `BT_STATE` і `STREAM_FMT` не відкидати.
2. **Аудіо важливіше за телеметрію.** Жодна робота на лінку не має права викликати розрив у
   потоці I2S.
3. **Мовчазний хост не є аварією.** Немає `PONG` — продовжувати грати й накопичувати стан.
   Exception: a pending FORMAT_READY transition remains silent until confirmed;
   missing PONG/READY never authorizes a new format to resume.
4. **Стан після скидання визначений.** Виходи GPIO піднімаються в те, що задав хост, і не
   раніше. До `GPIO_CFG` усі свої лінії тримати входами.
5. Логи йдуть у `LOG`, а не тільки в UART консоль.

---

## Поза межами

Екрана немає. Мережі немає. Файлів немає. Вебінтерфейсу немає. Нічого з цього не додавати.

---

## Піни

Підтверджено замовником 18.09.2026: брати зручні й безпечні, і щоб лежали **поруч на
гребінці** DevKitC.

Чого на ESP32 чіпати не можна: GPIO6-11 зайняті флешем, GPIO34-39 лише входи, а GPIO0, 2, 5,
12, 15 це strapping і виходами їх робити не можна.

| Сигнал | GPIO | де на гребінці |
|---|---|---|
| I2S BCLK | 25 | лівий ряд, три поспіль |
| I2S WS | 26 | ↑ |
| I2S DOUT | 27 | ↑ |
| UART TX до хоста | 17 | правий ряд, дві поспіль |
| UART RX від хоста | 16 | ↑ |

На DevKitC-32 лівий ряд іде ... 32, 33, **25, 26, 27**, 14, 12 ..., а правий ... 19, 18, 5,
**17, 16**, 4 ... Тобто I2S знімається одним трилінійним шлейфом, UART другим.

### Що лишається розширювачу

| | |
|---|---|
| Вхід або вихід | 13, 14, 18, 19, 21, 22, 23, 32, 33 |
| Тільки вхід | 34, 35, 36, 39 |

Скільки з них реально віддавати і що на них повісити — **вирішувати не зараз**. Пристрій
оголошує свій набір через `GPIO_MAP` уже в рантаймі, тому додати лінію потім це один рядок у
таблиці прошивки, без правки протоколу і без змін на боці хоста. Почати з порожнього набору
цілком нормально.

---

## Відкриті питання до замовника

Жодного, що блокує.

## LHDC V5 extension (customer request, 2026-09-19)

Pinned Apache-2.0 decoder and local adapter: see `LHDC_V5_2026-09-19.md`.
Rates: 44.1/48/96 kHz; stereo, 16/24 source bits, 5 ms framing. No 192 kHz,
lossless, LL or spatial features are advertised. 96 kHz needs host HELLO 1.4
with RELOCK, FORMAT_READY and 96KHZ; no resampling or forced widening is added.
Runtime performance and audio fidelity need bench verification with an LHDC V5 source.

## Cover integration (2026-09-20)

Protocol 1.6 streams linked JPEG thumbnails into host memory with RESET/DATA/END,
gated by host bits 1+6 and minor >=6. Maximum 256 KiB, explicit length/CRC completion,
bounded nonblocking UART enqueue and 16-KiB heap reserve. See `BT_COVER_2026-09-20.md`.

Since 2026-09-21 (hardening stage 10.3) this is the only cover path. The 48-KiB
collection path that served 1.5 hosts took the decoder's heap in 1000-byte pieces
on a board with about 47 KiB free while LHDC plays, and the bench showed `ring full`
bursts right after every collected image. A host announcing minor < 6, or without
bits 1 and 6, receives no cover messages at all; it is not offered the legacy
`0x14 COVER` either. Layouts and numbering are unchanged.

Since 2026-09-22 (stage 10.7, protocol 1.7) a host that received an image broken may
ask for it again with `COVER_RETRY 0x24`, and device capability bit 6 is what says
this build answers. A retry repeats the metadata read and the OBEX GET under a new
track generation -- the host needs a new one, because a repeated `COVER_RESET` pair is
idempotent by contract. Two repeats per image, counted from the last AVRCP track
change or the last completed transfer: one dropped frame is what this is for, and a
link that loses frames steadily is a different fault. A host that does not ask gets
exactly the 1.6 behaviour, an empty tile.

## Налаштування Bluetooth з хоста (протокол 1.8, 2026-09-25)

Запит замовника: налаштування Bluetooth на екрані панелі — код спарювання, видалення спарених,
увімкнення й вимкнення кодеків, ім'я. Код — «на екрані»: той самий шестизначний код на телефоні
й на панелі, людина підтверджує на панелі. Розкладки й правила — у `PROTOCOL.md`, розділ 1.8.

Що тримає пристрій, у NVS:

- налаштування: `radio`, `discoverable`, `auto_reconnect`, увімкнені кодеки, ім'я;
- власну таблицю спарених: адреса, ім'я, порядок підключень. У сховищі бондів Bluedroid є лише
  ключ, його тип, довжина PIN і прапорець Secure Connections, імен там немає.

Спосіб спарювання йде за останнім `HELLO` хоста: з бітом 7 — DisplayYesNo і запит до хоста, без
нього — NoInputNoOutput і згода без запиту, як у 1.7.

Що для цього треба змінити в стеку, бо стоковий Bluedroid 5.5.5 цього не дає:

- **Just Works до застосунку.** `btc_dm_sp_cfm_req_evt()` у `btc_dm.c` на Just Works відповідає
  «так» сам («just work, not show to users»), і застосунок про такий запит не дізнається.
  `kind` 1 без правки не виникне ніколи, і таке спарювання пройде повз панель. Правка —
  окремим патчем серії: передати запит застосунку з ознакою Just Works та іменем телефона, яке
  BTA вже прочитав.
- **Прихований режим.** Невидимість сама не зупиняє спарювання (`BTM_ALLOW_CONN_IF_NONDISCOVER`
  = TRUE): хто знає адресу, спарюється далі. Тому `discoverable` 0 — це ще й
  `BTM_SetPairableMode(FALSE, FALSE)`: нові спарювання відхиляються, спарені телефони
  підключаються своїм ключем. Не `connect_only_paired`: він пропускає лише ключі з захистом від
  MITM, а кожен бонд, створений у режимі 1.7 (NoInputNoOutput), без нього.
- **Перемикач LHDC V5.** AAC, aptX і aptX HD уже вимикаються на льоту
  (`A2DP_SinkOffer*`), LHDC V5 — ні. Такий самий перемикач — у `bt_overlay`, без патча.

Кодеки застосовуються тим самим перереєструванням кінцевих точок, що й пізній `HELLO`
(`main/endpoint_policy.h`). Воно працює лише з патчем 0006: без нього A2DP після першого ж
перереєстрування зникав до перезавантаження.
