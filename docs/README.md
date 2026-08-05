# Документация zkgram

1. [Архитектура](#1-архитектура)
2. [Python мост (интеграция с CryptoLayer)](#2-python-мост-интеграция-с-cryptolayer)
3. [UI плагины: архитектура и реализация](#3-ui-плагины-архитектура-и-реализация)
4. [Сборка](#4-сборка)
5. [Расширение проекта (модули)](#5-расширение-проекта-модули)

---

## 1. Архитектура

### Принцип zero knowledge

Telegram (через TDLib) в этой архитектуре недоверенный "провод", ровно как в [CryptoLayer](https://github.com/igmunv/cryptolayer): он должен видеть только шифротекст и никогда ключи или открытый текст. Чтобы это гарантировалось не договорённостью, а структурой кода, проект жёстко разделён на три слоя с однонаправленными зависимостями.

```
        src/core/  (единственный слой, знающий про оба других)
        /       \
  src/crypto/   src/telegram/
  (ключи,        (TDLib,
   шифрование)    сеть)
```

**`src/crypto/`**: zero knowledge ядро. Принимает открытый текст, файлы и ключи, отдаёт байты шифротекста, и наоборот при расшифровке. Инвариант: ни один файл здесь не должен включать что либо из `src/telegram/`, этот слой не имеет понятия о существовании Telegram, TDLib или сети.

**`src/telegram/`**: транспортный слой. Обёртка над TDLib: отправка и приём уже готовых байт как сообщений и документов. Инвариант: ни один файл здесь не должен включать что либо из `src/crypto/`, этот слой никогда не видит ключи или открытый текст, только непрозрачные `Bytes`.

**`src/core/`**: оркестрация. Единственное место, которому разрешено знать про `crypto` и `telegram` одновременно. Держит жизненный цикл каждой переписки (одна переписка это один `chatId`), ключи, и связывает `plaintext -> crypto::CryptoLayer -> Bytes -> telegram::TelegramClient` в обе стороны. Наружу (`UiProvider`) отдаёт уже расшифрованный текст и пути к файлам, сам UI ничего не знает ни про крипто, ни про TDLib.

### Почему это важно

Если в транспортном слое (TDLib интеграция, разбор сетевых ответов и так далее) найдут баг или его скомпрометируют, атакующий физически не сможет достать ключи или открытый текст, потому что их там просто никогда не было.

### Поток данных

**Отправка:** `core::Session::sendText/sendFile(chatId, ...)` вызывает `crypto::CryptoLayer::sendText/sendFile` для переписки этого чата (Python `cryptolayer.CryptoLayer`, шифрование), это идёт через `Callbacks::sendBytes/sendFile` в `telegram::TelegramClient::sendBytes/sendFile(chatId, ...)`.

**Приём:** `telegram::TelegramClient` получает колбэк из TDLib update лупа с chat id сообщения, `core::Session` находит по этому id нужную переписку и передаёт байты в её `Callbacks::registerReceiver`, дальше идёт Python `cryptolayer.CryptoLayer` (расшифровка, проверка подписи), и наконец `Callbacks::onTextReceived/onFileReceived` вызывает `UiProvider::onTextReceived/onFileReceived(chatId, ...)`.

---

## 2. Python мост (интеграция с CryptoLayer)

`src/crypto/` не реализует крипто примитивы заново на C++, вместо этого он встраивает Python интерпретатор (через [pybind11](https://github.com/pybind/pybind11), embed режим) и напрямую вызывает методы настоящего [`cryptolayer.CryptoLayer`](https://github.com/igmunv/cryptolayer/blob/main/src/crypto_layer.py): ключи ECDH/SECP256R1, AES 256 GCM, подписи ECDSA, WordCoder. Это та же библиотека, что используется в `cryptolayer-cli` и Python модулях.

Один процесс zkgram может держать несколько независимых объектов `CryptoLayer` одновременно, по одному на каждую активную зашифрованную переписку. Идентичность узла (подписывающий ключ) при этом одна на весь `data_dir` и общая для всех переписок, а состояние обмена ключами у каждого объекта своё.

### Компоненты моста

- **`src/crypto/python_bridge.hpp/.cpp`** владеет embedded интерпретатором (`py::scoped_interpreter`) на весь срок жизни процесса, прописывает в `sys.path` соседние Python репозитории:
  - `../cryptolayer/src` (сам движок `CryptoLayer`)
  - `../cryptolayer-module-interface` (`BaseModule`, `Credential`)
  - `python/` (локальный шим `python/bridge.py`)

  По умолчанию раскладка репозиториев ожидается такой же, как в `C:/Users/User/cryptolayer/`, где `zkgram`, `cryptolayer` и `cryptolayer-module-interface` лежат рядом друг с другом. Если у вас иначе, не нужно править `python_bridge.cpp`: путь настраивается через CMake опцию `ZKGRAM_CRYPTOLAYER_ROOT` (по умолчанию `..`), например `-DZKGRAM_CRYPTOLAYER_ROOT=D:/repos`.

- **`python/bridge.py`**: Python шим с двумя классами:
  - `CppUiProvider(UIProvider)` пробрасывает вызовы `UIProvider` (см. `cryptolayer/src/UIProvider.py`) в C++ через словарь колбэков.
  - `CppModule(BaseModule)`: `BaseModule`, чей `Sender`/`Listener` не ходят ни в какой мессенджер сами, а вызывают C++ колбэки (`send_bytes`, `send_file`, `register_receiver`), транспортом для него служит `telegram::TelegramClient`.

- **`src/crypto/crypto_layer.hpp/.cpp`**: C++ фасад: собирает `CppUiProvider`/`CppModule` из `Callbacks` (см. ниже), импортирует `crypto_layer.CryptoLayer` и создаёт его экземпляр. Наружу отдаёт только `init()`, `sendText()`, `sendFile()`, `stop()`.

### Callbacks: единственная граница между crypto и telegram

`crypto::CryptoLayer::Callbacks`: набор `std::function`, через который `src/core/session.cpp` связывает крипто мост с `telegram::TelegramClient`, не давая `src/crypto/` и `src/telegram/` включать друг друга напрямую (см. §1). Ключевые поля: `sendBytes`/`sendFile` (исходящее), `registerReceiver` (входящее, принимает Python колбэки `ingester`/`file_ingester` и конвертирует их в C++), плюс UI колбэки: `updateStatus`, `onTextReceived`, `onFileReceived`, `checkSignatures`, `onReady`, `onPingTimeout`, `onDisconnect`.

### GIL (Global Interpreter Lock)

TDLib дёргает колбэки из своих собственных потоков, не из потока, создавшего интерпретатор. Любой код, который в итоге вызывает Python (например колбэк из `registerReceiver`, дошедший до `pyOnBytes`/`pyOnFile`), обязан сначала взять `py::gil_scoped_acquire gil;`. Это уже сделано в `crypto_layer.cpp` для всех точек входа в Python. При добавлении новых точек соприкосновения C++ и Python не забывайте про это правило.

### Как проверить, что мост живой

TDLib обязательна для сборки (см. §4), поэтому нужен и `CMAKE_PREFIX_PATH`/`Td_DIR`:

```bash
cmake -S . -B build -DPYBIND11_FINDPYTHON=ON -DPython3_EXECUTABLE=<путь_до_python.exe> -DCMAKE_PREFIX_PATH=<путь_до_TDLib>
cmake --build build --config Release
./build/Release/zkgram.exe   # запускать из корня zkgram/, чтобы относительные пути sys.path резолвились
```

`src/telegram/telegram_client.cpp` реализован поверх `td::ClientManager` (тот же подход, что и в `example/cpp/td_example.cpp` в самом TDLib): фоновый поток крутит `receive()`, авторизация (телефон, код, пароль) идёт через колбэки, которые `core::Session` связывает с `UiProvider::requestCredential`. Мост собран, скомпилирован и проверен на реальном аккаунте: авторизация, список чатов, отправка и приём сообщений и файлов работают.

---

## 3. UI плагины: архитектура и реализация

### Что такое `UiProvider`

`zkgram::core::UiProvider` (`src/core/ui_provider.hpp`) это единственная точка контакта между `core::Session` и любым UI: консолью, GUI, интерфейсом бота и так далее. `core::Session` знает только про этот интерфейс, никогда про конкретную реализацию, то же самое разделение, что и между `crypto`/`telegram`, только на границе движка и пользователя. Интерфейс покрывает все UI сигналы, которые отдаёт `crypto::CryptoLayer::Callbacks`, плюс список чатов на уровне всего приложения. Каждый сигнал переписки (кроме списка чатов) принимает `ConversationId` (алиас `int64_t`, совпадает с id чата в Telegram), потому что в приложении одновременно может идти несколько зашифрованных переписок:

```cpp
class UiProvider {
public:
    virtual ~UiProvider() = default;

    virtual void onInit() {}
    virtual void onChatListUpdated(const std::vector<ChatListEntry>& chats) {}

    virtual void onStatus(ConversationId conversation, const std::string& stage, const std::string& message) = 0;
    virtual void onTextReceived(ConversationId conversation, const std::string& text) = 0;
    virtual void onFileReceived(ConversationId conversation, const std::string& filePath) = 0;

    virtual void onReady(ConversationId conversation) = 0;
    virtual void onPingTimeout(ConversationId conversation) = 0;
    virtual void onDisconnect(ConversationId conversation) = 0;

    virtual bool confirmSignatures(ConversationId conversation, const std::string& mySign,
                                    const std::string& companionSign) = 0;
    virtual std::string requestCredential(const std::string& prompt, const std::string& dataType) = 0;
};
```

Соответствие методов `UiProvider` и полей `crypto::CryptoLayer::Callbacks`:

| `Callbacks`         | `UiProvider`                          | Когда срабатывает                          |
|---------------------|----------------------------------------|---------------------------------------------|
| `updateStatus`      | `onStatus(conversation, stage, message)` | Изменение стадии этой переписки            |
| `onTextReceived`    | `onTextReceived(conversation, text)`   | Пришло расшифрованное текстовое сообщение    |
| `onFileReceived`    | `onFileReceived(conversation, filePath)` | Пришёл и расшифрован файл                  |
| `checkSignatures`   | `confirmSignatures(conversation, mySign, companionSign)` | Нужно показать пользователю сверку подписей и получить решение |
| `onReady`           | `onReady(conversation)`                | Эта переписка установлена и готова к обмену |
| `onPingTimeout`     | `onPingTimeout(conversation)`          | Собеседник не ответил на пинг вовремя       |
| `onDisconnect`      | `onDisconnect(conversation)`           | Эта переписка завершена или разорвана       |
| `request_data` (Python `UIProvider.request_data`) | `requestCredential(prompt, dataType)` | CryptoLayer запрашивает данные у пользователя (например пароль), общий для всего приложения, не для отдельной переписки |
| (список чатов TDLib) | `onChatListUpdated(chats)`            | Список чатов из Telegram обновился          |

`onInit()` вызывается один раз, до `core::Session::start()`, и даёт UI плагину возможность подготовить своё окно или event loop до начала подключения.

### Модель выбора плагина: компайл тайм, не рантайм

Плагин выбирается на этапе конфигурации CMake опцией `ZKGRAM_UI` (`console` по умолчанию, либо `qt`) и линкуется в `zkgram.exe` статически. Рантайм загрузка `.dll` намеренно не используется:

- `UiProvider` это C++ vtable интерфейс со `std::string`/`std::function` в сигнатурах, не ABI стабильный между версиями компилятора, поэтому загрузка чужого `.dll` в рантайме потребовала бы отдельного стабильного C ABI слоя.
- Проекту не требуется горячая замена UI без пересборки, нет задачи маркетплейса плагинов.
- Если такая потребность появится позже, поменяется только механизм выбора плагина (загрузчик вместо статической линковки), сам интерфейс `UiProvider` останется прежним.

### Правило потоков и GIL

Методы `UiProvider` вызываются из той же цепочки колбэков, что и обращения к Python (см. §2 про GIL), не из потока UI фреймворка. Для методов без возвращаемого значения (`onStatus`, `onReady` и так далее) это означает: не делать в них тяжёлой синхронной работы, а как можно быстрее передать событие в свой UI поток.

Для `confirmSignatures` и `requestCredential`, которым нужно вернуть значение вызывающей стороне, синхронный блокирующий вызов допустим, но только ожидание явного ввода пользователя, никогда не сетевые или файловые операции. Правильный способ реализовать это при собственном event loop (Qt или любой другой), это маршалинг вызова на поток UI с ожиданием ответа (в Qt это `QMetaObject::invokeMethod` с `Qt::BlockingQueuedConnection`), а не прямой вызов виджетов из чужого потока.

### Минимальный пример: `ConsoleUiProvider`

`src/main.cpp`, собирается опцией `-DZKGRAM_UI=console` (по умолчанию). Консольный UI намеренно проще Qt плагина: он держит только одну активную переписку за раз, а не полный список чатов, companion спрашивается один раз через stdin после входа.

```cpp
class ConsoleUiProvider : public zkgram::core::UiProvider {
public:
    void onStatus(zkgram::core::ConversationId conversation, const std::string& stage,
                  const std::string& message) override {
        std::cout << "[" << conversation << "][" << stage << "] " << message << "\n";
    }
    void onTextReceived(zkgram::core::ConversationId conversation, const std::string& text) override {
        std::cout << "[" << conversation << "] peer: " << text << "\n";
    }
    void onFileReceived(zkgram::core::ConversationId conversation, const std::string& filePath) override {
        std::cout << "[" << conversation << "] peer sent a file: " << filePath << "\n";
    }
    void onReady(zkgram::core::ConversationId conversation) override {
        std::cout << "[" << conversation << "] ready\n";
    }
    void onPingTimeout(zkgram::core::ConversationId conversation) override {
        std::cout << "[" << conversation << "] ping timeout\n";
    }
    void onDisconnect(zkgram::core::ConversationId conversation) override {
        std::cout << "[" << conversation << "] disconnected\n";
    }

    bool confirmSignatures(zkgram::core::ConversationId conversation, const std::string& mySign,
                            const std::string& companionSign) override {
        std::cout << "[" << conversation << "] My signature: " << mySign << "\n";
        std::cout << "[" << conversation << "] Companion signature: " << companionSign << "\n";
        std::cout << "Confirm match? [y/N]: ";
        std::string answer;
        std::getline(std::cin, answer);
        return answer == "y" || answer == "Y";
    }

    std::string requestCredential(const std::string& prompt, const std::string& dataType) override {
        std::cout << prompt << " (" << dataType << "): ";
        std::string value;
        std::getline(std::cin, value);
        return value;
    }
};
```

### Qt плагин: полноценный многочатовый клиент

`src/ui/qt/`, собирается опцией `-DZKGRAM_UI=qt`. Файлы:

- `qt_ui_provider.hpp/.cpp`: `QtUiProvider`, реализация `UiProvider` поверх Qt Widgets, маршалит каждый вызов на GUI поток.
- `main_window.hpp/.cpp`: `MainWindow`. Слева сайдбар со списком всех чатов из TDLib (обновляется через `onChatListUpdated`), справа выбранная переписка: шапка с именем собеседника и статус пилюлей соединения, список сообщений (свои, чужие, системные, пузырьками, картинки показываются превью прямо в чате), поле ввода, кнопки "Send", "File" и микрофон для голосовых. Если для выбранного чата ещё не начата зашифрованная переписка, вместо поля ввода показывается кнопка "Start encrypted session": это и есть явное включение шифрования по чату, а не автоматический запуск при входе. Правый клик по сообщению открывает Copy/Edit/Forward (см. §"Design принципы" ниже про честные ограничения Edit/Forward).
- `main_qt.cpp`: точка входа. Единственный слайд входа перед запуском это локальный пароль, который отпирает вашу локальную идентичность (ключ подписи) на этом компьютере, он не имеет отношения к паролю Telegram. После входа `core::Session` сам загружает список чатов, никакого ID собеседника вводить заранее не нужно.

Внешний вид (цвета, отступы, скругления) вынесен в `src/ui/qt/style.qss`, обычный Qt Style Sheet с CSS подобным синтаксисом. Файл лежит рядом с самим плагином, а не в общем `resources/`, потому что принадлежит именно этому UI плагину, а не проекту в целом, и встраивается в exe через Qt Resource System (`resources.qrc`), так что правки требуют пересборки. Пузырьки сообщений красятся через QSS селекторы по динамическому свойству (`QLabel[messageKind="outgoing"]` и так далее), а не через `setStyleSheet` в C++, поэтому все цвета собраны в одном месте.

### Голосовые сообщения

Голосовое это просто ещё один файл, отдельной архитектуры под него не потребовалось: кнопка микрофона в `MainWindow` пишет звук через `QMediaRecorder`/`QAudioInput`/`QMediaCaptureSession` (модуль `Qt6::Multimedia`, добавлен в `CMakeLists.txt` для `ZKGRAM_UI=qt`) в `.m4a` файл во временную папку (`QStandardPaths::TempLocation`). После остановки записи (`QMediaRecorder::recorderStateChanged` до `StoppedState`) файл уходит тем же путём, что и обычный "Send File": `core::Session::sendFile`, шифрование, `TelegramClient::sendFile`. Иконка микрофона нарисована в коде через `QPainter` (капсула и дуга подставка), без внешнего файла ассета, при записи перекрашивается в красный.

Ещё одна деталь, специфичная именно для этого проекта: `main_window.hpp`/`main_qt.cpp` в итоге включают `Python.h` (через `core/session.hpp`, `crypto_layer.hpp`, pybind11), а в `Python.h` есть структурное поле, буквально называющееся `slots`. Макросы Qt `slots`/`signals`/`emit` с этим конфликтуют, поэтому для Qt таргета в `CMakeLists.txt` включён `QT_NO_KEYWORDS`, а наш код использует `Q_SLOTS`/`Q_SIGNALS`/`Q_EMIT` вместо голых ключевых слов.

Единственная нетривиальная деталь самого плагина: `UiProvider` вызывается не из Qt GUI потока, поэтому `QtUiProvider` не трогает виджеты напрямую, а маршалит вызовы через `QMetaObject::invokeMethod`:

```cpp
void QtUiProvider::onStatus(zkgram::core::ConversationId conversation, const std::string& stage,
                             const std::string& message) {
    QMetaObject::invokeMethod(window_, "appendConversationStatus", Qt::QueuedConnection,
                               Q_ARG(qlonglong, conversation), Q_ARG(QString, QString::fromStdString(stage)),
                               Q_ARG(QString, QString::fromStdString(message)));
}
```

Для `confirmSignatures`/`requestCredential`, которым нужно значение обратно, используется `Qt::BlockingQueuedConnection` (или прямой вызов, если вызов пришёл из самого GUI потока, чтобы не поймать самоблокировку), смотрите `qt_ui_provider.cpp`.

Сборка Qt плагина:

```bash
cmake -S . -B build-qt -DPYBIND11_FINDPYTHON=ON -DZKGRAM_UI=qt -DCMAKE_PREFIX_PATH=<путь_до_Qt6>
cmake --build build-qt --config Release
```

или одной командой (см. §4): `.\build.ps1 -Ui qt -QtPrefix <путь_до_Qt6>`.

### Почему не `lib_ui`/`RpWidget` (UI библиотека Telegram Desktop)

Рассматривалась идея переиспользовать собственный набор виджетов Telegram Desktop (`lib_ui`, `RpWidget`) ради той же скорости и плавности анимаций, что и в оригинальном клиенте. По итогам предметного изучения решено отложить этот вариант:

- `lib_ui` живёт в GitHub организации `desktop-app` вместе с как минимум `lib_base`, `lib_rpl`, `lib_crl`, `codegen`, `cmake_helpers`, то есть подключение тянет за собой более 10 отдельных репозиториев, а не два три.
- Перед компиляцией любого кода, использующего `RpWidget`, требуется отдельный шаг кодогенерации: `.style` файлы компилируются инструментом `codegen`, который сам нужно собрать заранее.
- Сборка ориентирована не на ванильный Qt, а на Qt, патченный самим проектом Telegram Desktop (репозиторий `patches`, привязка к конкретной версии `qtbase`).
- Прецедентов самостоятельного использования `lib_ui` вне полного дерева исходников tdesktop не найдено.

Это делает интеграцию многонедельной и рискованной задачей, а не тем, что можно сделать за один заход. Решение отложено, а не отвергнуто навсегда: если позже появится конкретная потребность, которую обычный Qt Widgets не закрывает, стоит пересмотреть вопрос заново, а не отказываться от него автоматически.

### Как добавить свой UI плагин

1. Создайте класс, наследующий `zkgram::core::UiProvider`, реализуйте все методы из §"Что такое UiProvider".
2. Заведите новое значение `ZKGRAM_UI` (например `tui`) и соответствующую ветку в `CMakeLists.txt` со своими исходниками и зависимостями по образцу ветки `qt`.
3. Добавьте свою точку входа (`main_<name>.cpp`), которая собирает `core::Session` с вашим `UiProvider`.
4. Если ваш UI живёт в своём event loop (не консольный ввод вывод), соблюдайте правило потоков выше: не трогайте состояние UI напрямую из методов `UiProvider`, маршальте вызов на свой поток.

### Design принципы для UI слоя

- **UI ничего не шифрует и ничего не знает о транспорте.** Если тянет добавить в UI класс логику вроде "а давай тут же и файл в Telegram отправим", это неправильное место, транспорт вызывается только через `core::Session::sendText/sendFile`.
- **UI тонкий и заменяемый.** И `ConsoleUiProvider`, и `QtUiProvider` не временные заглушки, а образец того, каким должен оставаться любой `UiProvider`: только преобразование событий в конкретный вывод или виджеты, без бизнес логики.

---

## 4. Сборка

### Зависимости

- CMake версии 3.20 и выше
- Компилятор с поддержкой C++20 (MSVC 19.3x или новее, GCC 11 или новее, Clang 13 или новее)
- Python 3.8 или новее с dev заголовками и библиотеками (для embedded интерпретатора, см. §2), pybind11 подтягивается автоматически через CMake `FetchContent`, отдельно ставить не нужно
- [TDLib](https://github.com/tdlib/td), обязательна для сборки (`find_package(Td REQUIRED)` в `CMakeLists.txt`), собранная из исходников или через vcpkg
- OpenSSL, обязательна, требуется и самой TDLib, и линкуется явно (`find_package(OpenSSL REQUIRED)`)

### TDLib

TDLib не поставляется как обычный пакет, её нужно собрать самостоятельно (или взять через vcpkg, если платформа и версия подходят).

- Официальная инструкция по сборке: https://github.com/tdlib/td#building
- Через vcpkg (проще на Windows): `vcpkg install tdlib`

После сборки или установки TDLib укажите её через `CMAKE_PREFIX_PATH` или `Td_DIR` при конфигурации.

### Одной командой (build.ps1)

`build.ps1` в корне репозитория оборачивает конфигурацию и сборку через `cmake` в одну команду и проверяет, что сборка запущена на Windows.

```powershell
.\build.ps1 -TdPrefix C:\tdlib                                              # консольный UI, Release
.\build.ps1 -Ui qt -TdPrefix C:\tdlib -QtPrefix C:\Qt\6.9.3\msvc2022_64      # Qt UI
```

Параметры: `-Ui console|qt` (по умолчанию `console`), `-Config` (по умолчанию `Release`), `-PythonExe` (путь к нужному `python.exe`), `-TdPrefix` (путь к сборке или установке TDLib), `-QtPrefix` (путь к установке Qt6, нужен только при `-Ui qt`), `-VcpkgBinDir` (нужен только при сборке TDLib с динамической линковкой OpenSSL/zlib, для обычной сборки не требуется), `-Clean` (удалить папку `build` перед конфигурацией). Скрипт не подавляет ошибки `cmake`, он просто оборачивает команды ниже.

### Сборка вручную

```bash
cmake -S . -B build -DPYBIND11_FINDPYTHON=ON -DPython3_EXECUTABLE=<путь_до_python.exe> -DCMAKE_PREFIX_PATH=<путь_до_TDLib>
cmake --build build --config Release
```

Флаг `-DPYBIND11_FINDPYTHON=ON` и явный `-DPython3_EXECUTABLE` нужны, чтобы pybind11 гарантированно использовал тот же Python, что и остальная сборка, иначе на системах с несколькими установками Python (например MSYS2 и python.org) CMake может найти не тот интерпретатор и упасть на `Python libraries not found`. `-DCMAKE_PREFIX_PATH` нужен, чтобы `find_package(Td REQUIRED)` нашёл вашу сборку или установку TDLib.

На Windows с vcpkg (для TDLib) вместо `CMAKE_PREFIX_PATH` можно подключить toolchain:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<путь_до_vcpkg>/scripts/buildsystems/vcpkg.cmake -DPYBIND11_FINDPYTHON=ON -DPython3_EXECUTABLE=<путь_до_python.exe>
```

### Запуск

```bash
./build/Release/zkgram.exe
```

Запускать из корня `zkgram/` (или так, чтобы `../cryptolayer/src` и `../cryptolayer-module-interface` резолвились от текущей директории), иначе Python мост не найдёт `cryptolayer`/`base_module` (см. §2).

---

## 5. Расширение проекта (модули)

Проект структурирован так, чтобы новую функциональность можно было добавлять, не нарушая изоляцию `src/crypto/` и `src/telegram/` (§1). Перед тем как добавлять код, определите, в какой из слоёв он на самом деле относится.

### Чек лист перед добавлением кода

1. Код работает только с байтами или ключами и не знает про сеть? Значит `src/crypto/`.
2. Код работает только с TDLib или сетью и не знает про ключи или открытый текст? Значит `src/telegram/`.
3. Код должен видеть и то, и другое, например решает, когда шифровать и что отправлять? Значит только `src/core/`.
4. Если код одновременно включает что то из `crypto/` и `telegram/` вне `src/core/`, это нарушение изоляции, вне зависимости от того, насколько удобнее было бы сделать иначе.

### Добавление нового крипто алгоритма или бэкенда

Новый код добавляется внутрь `src/crypto/`. Публичный интерфейс наружу только через `crypto::CryptoLayer` (`init`/`sendText`/`sendFile`/`stop` и что появится позже). Никаких Telegram специфичных типов в сигнатурах.

### Добавление альтернативного транспорта (не TDLib)

Хотя сейчас транспорт это TDLib, `src/telegram/` спроектирован как пример адаптера: при необходимости завести второй мессенджер или транспорт стоит создать параллельную директорию по образу `src/telegram/` (например `src/discord/`) с тем же контрактом (`connect()`, `sendBytes(chatId, Bytes)`, `sendFile(chatId, path)`, `onBytesReceived(callback)`, `onFileReceived(callback)`), не добавлять туда ничего из `src/crypto/`, и подключать в `src/core/session.*` так же, как сейчас подключён `telegram::TelegramClient`.

### Добавление своего UI

См. §3, отдельная глава. Механизм регистрации плагина это компайл тайм опция CMake `ZKGRAM_UI`.

### Стиль кода

- `snake_case` для файлов, `PascalCase` для классов, `camelCase` для методов и полей.
- Каждый новый публичный класс или файл в соответствующем `namespace zkgram::<layer>`.
