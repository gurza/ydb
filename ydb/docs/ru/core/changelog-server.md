# Список изменений {{ ydb-short-name }} Server

## Версия 23.1 {#23-1}

Дата выхода 5 мая 2023. Для обновления до версии 23.1 перейдите в раздел [Загрузки](downloads/index.md#ydb-server).

**Функциональность:**

* Добавлено [первоначальное сканирование таблицы](concepts/cdc.md#initial-scan) при создании потока изменений CDC. Теперь можно выгрузить все данные, которые существуют на момент создания потока.
* Добавлена возможность [атомарной замены индекса](best_practices/secondary_indexes.md#atomic-index-replacement). Теперь можно атомарно и прозрачно для приложения подменить один индекс другим заранее созданным индексом. Замена выполняется без простоя.
* Добавлен [аудитный лог](cluster/audit-log.md) — поток событий, который содержит информацию обо всех операциях над объектами {{ ydb-short-name }}.

**Производительность:**

* Улучшены форматы передачи данных между стадиями исполнения запроса, что ускорило SELECT на запросах с параметрами на 10%, на операциях записи — до 30%.
* Добавлено [автоматическое конфигурирование](deploy/configuration/config.md#autoconfig) пулов акторной системы в зависимости от их нагруженности. Это повышает производительность за счет более эффективного совместного использования ресурсов ЦПУ.
* Оптимизирована логика применения предикатов — выполнение ограничений с использованием OR и IN с параметрами автоматический переносится на сторону DataShard.
* Для сканирующих запросов реализована возможность эффективного поиска отдельных строк с использованием первичного ключа или вторичных индексов, что позволяет во многих случаях значительно улучшить производительность. Как и в обычных запросах, для использования вторичного индекса необходимо явно указать его имя в тексте запроса с использованием ключевого слова `VIEW`.
* Реализовано кеширование графа вычисления при выполнении запросов, что уменьшает потребление ЦПУ при его построении.

**Исправления ошибок:**

* Исправлен ряд ошибок в реализации распределенного хранилища данных. Мы настоятельно рекомендуем всем пользователям обновиться на актуальную версию.
* Исправлена ошибка построения индекса на not null колонках.
* Исправлен подсчет статистики при включенном MVCC.
* Исправлены ошибки с бэкапами.
* Исправлена гонка во время сплита и удаления таблицы с CDC.

## Версия 22.5 {#22-5}

Дата выхода 7 марта 2023. Для обновления до версии **22.5** перейдите в раздел [Загрузки](downloads/index.md#ydb-server).

**Что нового:**

* Добавлены [параметры конфигурации потока изменения](yql/reference/syntax/alter_table.md#changefeed-options) для передачи дополнительной информации об изменениях в топик.
* Добавлена поддержка [переименования для таблиц](concepts/datamodel/table.md#rename) с включенным TTL.
* Добавлено [управление временем хранения записей](concepts/cdc.md#retention-period) для потока изменений.

**Исправления ошибок и улучшения:**

* Исправлена ошибка при вставке 0 строк операцией BulkUpsert.
* Исправлена ошибка при импорте колонок типа Date/DateTime из CSV.
* Исправлена ошибка импорта данных из CSV с разрывом строки.
* Исправлена ошибка импорта данных из CSV с пустыми значениями.
* Улучшена производительность Query Processing (WorkerActor заменен на SessionActor).
* Компактификация DataShard теперь запускается сразу после операций split или merge.

## Версия 22.4 {#22-4}

Дата выхода 12 октября 2022. Для обновления до версии **22.4** перейдите в раздел [Загрузки](downloads/index.md#ydb-server).

**Что нового:**

* {{ ydb-short-name }} Topics и Change Data Capture (CDC):
  * Представлен новый Topic API. [Топик](concepts/topic.md) {{ ydb-short-name }} — это сущность для хранения неструктурированных сообщений и доставки их различным подписчикам.
  * Поддержка нового Topic API добавлена в [{{ ydb-short-name }} CLI](reference/ydb-cli/topic-overview.md) и [SDK](reference/ydb-sdk/topic.md). Topic API предоставляет методы потоковой записи и чтения сообщений, а также управления топиками.
  * Добавлена возможность [захвата изменений данных таблицы](concepts/cdc.md) с отправкой сообщений об изменениях в топик.

* SDK:
  * Добавлена возможность взаимодействовать с топиками в {{ ydb-short-name }} SDK.
  * Добавлена официальная поддержка драйвера database/sql для работы с {{ ydb-short-name }} в Golang.

* Embedded UI:
  * Поток изменений CDC и вторичные индексы теперь отображаются в иерархии схемы базы данных как отдельные объекты.
  * Улучшена визуализация графического представления query explain планов.
  * Проблемные группы хранения теперь более заметны.
  * Различные улучшения на основе UX-исследований.

* Query Processing:
  * Добавлен Query Processor 2.0 — новая подсистема выполнения OLTP-запросов со значительными улучшениями относительно предыдущей версии.
  * Улучшение производительности записи составило до 60%, чтения до 10%.
  * Добавлена возможность включения ограничения NOT NULL для первичных ключей в YDB во время создания таблиц.
  * Включена поддержка переименования вторичного индекса в режиме онлайн без остановки сервиса.
  * Улучшено представление query explain, которое теперь включает графы для физических операторов.

* Core:
  * Для read-only транзакций добавлена поддержка консистентного снапшота, который не конфликтует с пишущими транзакциями.
  * Добавлена поддержка BulkUpsert для таблиц с асинхронными вторичными индексами.
  * Добавлена поддержка TTL для таблиц с асинхронными вторичными индексами.
  * Добавлена поддержка сжатия при экспорте данных в S3.
  * Добавлен audit log для DDL statements.
  * Поддержана аутентификация со статическими учетными данными.
  * Добавлены системные таблицы для диагностики производительности запросов.
