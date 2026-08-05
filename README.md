# zkgram

C++ Telegram клиент на базе [TDLib](https://github.com/tdlib/td) с интегрированным [CryptoLayer](https://github.com/igmunv/cryptolayer). Сообщения шифруются до попадания в Telegram, поэтому сам Telegram видит только непрозрачный шифротекст. Подробности архитектуры, интеграции с CryptoLayer и того, как расширять проект, смотрите в [`docs/README.md`](docs/README.md).

Используется модифицированная версия библиотеки [CryptoLayer](https://github.com/igmunv/cryptolayer): в неё добавлена поддержка Telegram и отправка зашифрованных файлов. Модификации и модуль для Telegram лежат в [selimann/cryptolayer-modules/Telegram](https://github.com/selimann/cryptolayer-modules/tree/main/Telegram).

## Возможности

Приложение работает как полноценный многопользовательский клиент, похожий на Telegram Desktop: слева список всех чатов из вашего аккаунта Telegram, справа переписка с выбранным собеседником. Шифрование включается отдельно для каждого чата кнопкой "Start encrypted session", оно не запускается автоматически при входе.

## Зависимости

Обязательно для любой сборки:

- Windows (проект сейчас собирается только под Windows, см. `docs/README.md`)
- [Git](https://git-scm.com/), нужен CMake (`FetchContent` тянет pybind11) и `install-tdlib.ps1`
- CMake версии 3.20 и выше
- Компилятор с поддержкой C++20: MSVC 19.3x или новее (Visual Studio 2022 Build Tools либо полная VS), это тот тулчейн, на котором проект реально собирался и проверялся
- Python 3.8 или новее с dev заголовками и библиотеками (интерпретатор для embedded pybind11 моста, сам pybind11 подтягивается автоматически через CMake `FetchContent`, ставить отдельно не нужно)
  - В этот же Python нужно поставить пакеты, которые использует Python ядро CryptoLayer: `pip install cryptography brotli`
- [TDLib](https://github.com/tdlib/td), обязательна, `find_package(Td REQUIRED)` в `CMakeLists.txt`. Готовых бинарников проект не публикует и не тянет сторонние прекомпилированные сборки TDLib, потому что это криптографический компонент и доверять чужому бинарнику без возможности проверить его происхождение не стоит. Собирается из исходников, см. `install-tdlib.ps1` ниже
- [OpenSSL](https://openssl.org/), обязательна и самой TDLib, и напрямую (`find_package(OpenSSL REQUIRED)` в `CMakeLists.txt`)

Только для сборки с `-DZKGRAM_UI=qt` (графический интерфейс на Qt, консольный вариант собирается по умолчанию и ничего из этого списка не требует):

- Qt6, модули `Widgets` и `Multimedia` (`Multimedia` нужен для записи голосовых сообщений). Нужна MSVC совместимая сборка Qt6 (официальный установщик Qt с китом `msvc2022_64` или через vcpkg). Сборка Qt6 из MSYS2/MinGW не подходит, она ABI несовместима с MSVC тулчейном проекта, подробности в `docs/README.md`

Опционально, только если нужно собрать TDLib с нуля:

- [vcpkg](https://github.com/microsoft/vcpkg), `install-tdlib.ps1` бутстрапит его сам при первом запуске, ставить отдельно не нужно. Через него собираются zlib, gperf и OpenSSL, если системного OpenSSL под MSVC ещё нет

## Установка TDLib

TDLib не поставляется готовыми бинарниками, её нужно собрать из исходников. `install-tdlib.ps1` делает это автоматически: бутстрапит vcpkg, ставит через него OpenSSL, zlib и gperf, затем собирает саму TDLib напрямую через CMake только в Release конфигурации (без Debug, потому что debug сборка TDLib с полной отладочной информацией легко занимает 7 и более гигабайт и может забить диск).

```powershell
.\install-tdlib.ps1
```

По умолчанию всё ставится в `C:\vcpkg`, `C:\tdlib-src`, `C:\tdlib-build`, `C:\tdlib-install`, пути настраиваются параметрами (см. `.\install-tdlib.ps1 -?` или сам файл скрипта). После успешной установки скрипт печатает готовую команду для `build.ps1`. Подробнее о параметрах смотрите в `docs/README.md`.

## Сборка и запуск

Одной командой (PowerShell, только Windows), после того как TDLib установлена:

```powershell
.\build.ps1 -Ui qt -TdPrefix "C:\tdlib-install;C:\vcpkg\installed\x64-windows" -QtPrefix "C:\Qt\6.9.3\msvc2022_64"
```

Запускать `zkgram.exe` нужно из корня репозитория (или так, чтобы `../cryptolayer` и `../cryptolayer-module-interface` резолвились от текущей директории), иначе Python мост не найдёт CryptoLayer. Ручная сборка через `cmake` напрямую, параметры `build.ps1` и объяснение опции `-DZKGRAM_UI` описаны в [`docs/README.md`](docs/README.md).

## Статус

Python мост к [CryptoLayer](https://github.com/igmunv/cryptolayer) рабочий и проверенный. TDLib транспорт собран, авторизация и отправка сообщений проверены вживую на реальном аккаунте. Список чатов, множественные одновременные зашифрованные переписки и явный запуск шифрования по кнопке реализованы и работают.

## Лицензия

Проект распространяется под [GNU GPL версии 3](https://www.gnu.org/licenses/gpl-3.0.html). Часть UI кода (`src/ui/qt/`) переиспользует элементы из [tdesktop](https://github.com/telegramdesktop/tdesktop), официального клиента Telegram Desktop, тоже распространяемого под GPLv3, что и требует такого лицензирования. Полный текст лицензии в файле [`LICENSE`](LICENSE).
