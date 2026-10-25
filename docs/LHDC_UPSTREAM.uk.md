[English](LHDC_UPSTREAM.md) · **Українська**

# Походження декодера LHDC V5

Факти станом на 2026-09-21, оновлені 2026-10-24. Цей файл записує, що відомо про те, звідки код і
на яких умовах; юридичних висновків тут немає.

- Джерело: https://github.com/WillyBilly06/LHDC-V5-Decoder
- Прибитий коміт: той, що в `lhdc.pin`, `cf857ce1a1429965e161e54adbc3f293fd9ce273`.
- **Підтягується, а не лежить у репозиторії.** З 2026-09-19 до 2026-10-24 приватний репозиторій,
  з якого відкрито цей, містив копію `decoder/`, `CMakeLists.txt` компонента і `README.md`, байт у
  байт. Перед відкриттям його історію переписано, тож жоден коміт тут їх не містить:
  `tools/setup.sh --with-lhdc` (`setup.ps1 -WithLhdc`) підтягує ці три шляхи на прибитому коміті в
  `components/lhdcv5_decoder/`, яку git ігнорує, а прошивка збирає кодек лише з `sdkconfig.lhdc`.
  Причина — у розділі про ліцензію нижче.
- Ніколи не переносилось: вкладене дерево IDF, застосунки, пропрієтарний енкодер, бінарні файли,
  декодер LDAC.
- Склейку з A2DP адаптовано з `a2dp_integration/` апстріму в `bt_overlay/`
  (`a2dp_vendor_lhdcv5*.{c,h}`), і вона лишається в цьому репозиторії: вона розбирає елемент
  можливостей A2DP цього кодека й обгортає API декодера, а витягнутих даних у ній немає. Кожен з
  цих файлів каже, звідки він.

## Пін переїхав, код ні

Апстрім переписав свою історію після 2026-09-21. Коміт, прибитий до того,
`676af03d2b7a2ed5c1f9bdca7bf8938cc85f704b`, більше не лежить на жодній з двох його гілок (`main`,
`sync-esp32-d0wd-with-main`), а коміт, якого не досягає жодна гілка, GitHub може будь-коли
прибрати збирачем сміття. `cf857ce1`, голова `main` на 2026-10-24, має ту саму дату й повідомлення,
а його `decoder/`, `CMakeLists.txt`, `README.md` і `a2dp_integration/` збігаються з `676af03d`
блоб у блоб (git-ідентифікатори всіх 24 файлів однакові). Копія, яку мав приватний репозиторій,
збігалася з обома.

## Ліцензія: що насправді є в репозиторії апстріму

- **Файлу ліцензії немає.** Репозиторій на прибитому коміті містить `CMakeLists.txt`, `README.md`,
  `idf_component.yml` і теки `WROVER_A2DP_Sink/`, `a2dp_integration/`, `decoder/`, `docs/`,
  `esp-idf-v6.1-codecs/`, `test/`. Ні `LICENSE`, ні `LICENSE.md`, ні `COPYING`, ні `NOTICE` немає
  на жодному рівні з того, що підтягується.
- **Єдине твердження про ліцензію — одне речення в `README.md`** (розділ "License"): "This decoder
  implementation is released under the **Apache License 2.0**. Note that this does **not** grant
  any rights to the LHDC codec itself, which is the property of Savitech; obtaining LHDC licensing
  for your use case is your responsibility."
- **Власне застереження README** (на початку файлу): "LHDC is a proprietary high-resolution
  Bluetooth codec developed by Savitech. This repository is an independent decoder implementation
  for interoperability/research. It contains no Savitech/Qualcomm binaries. You are responsible for
  ensuring you have the rights to use LHDC in your jurisdiction and product."
- **Сирці декодера не мають ні рядка копірайту, ні ідентифікатора SPDX.** Жоден файл під
  `decoder/` не називає ні автора, ні року, ні ліцензії.
- **`decoder/lhdc_tables.c` сам каже, звідки він**: у його заголовку написано "REAL EXTRACTED DATA
  FROM liblhdcv5.so (LHDC V5 Encoder Binary)", тобто таблиці смуг, вікон і квантування витягнуто з
  пропрієтарної бібліотеки енкодера Savitech, а не виведено зі специфікації. Дозвіл з README
  апстріму не може говорити за права Savitech на них.

## Що колись казав приватний репозиторій і чому вже не каже

До 2026-09-21 тека декодера там мала файл `LICENSE` з текстом Apache-2.0. Він був не з апстріму:
`vendor-review/integrate_lhdc.py` скопіював на його місце власний `LICENSE` з ESP-IDF, і в копії
лишився незаповнений рядок шаблону `Copyright [yyyy] [name of copyright owner]`. Текст ліцензії,
якого автор апстріму ніколи не публікував і який нікому не приписаний, нічого не документує, тож
його прибрали. Речення з README вище — це весь дозвіл апстріму, і саме цей файл зберігає цей факт.
Див. також `THIRD_PARTY_NOTICES.uk.md` у корені репозиторію.
