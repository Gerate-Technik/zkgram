"""
Python-шим, связывающий настоящий CryptoLayer (см. ../../cryptolayer, .
./../cryptolayer-module-interface) с C++ через колбэки.

Ничего не импортирует и не знает про TDLib напрямую — принимает только
словари с callable'ами, которые на C++-стороне (src/crypto/crypto_layer.cpp)
собираются из std::function, в конечном счёте ведущих в src/core/session.cpp.
"""

from base_module import BaseModule, Credential
from UIProvider import UIProvider


class CppUiProvider(UIProvider):
    """Пробрасывает колбэки CryptoLayer.UIProvider в C++."""

    def __init__(self, callbacks: dict):
        self._cb = callbacks

    def request_data(self, prompt: str, data_type: type):
        value = self._cb["request_data"](prompt, data_type.__name__)
        return data_type(value)

    def update_status(self, stage: str, message: str, status_type: str = "in_progress"):
        self._cb["update_status"](stage, message, status_type)

    def on_text_received(self, timestamp: int, text: str):
        self._cb["on_text_received"](timestamp, text)

    def on_file_received(self, timestamp: int, file_path: str, original_name: str):
        self._cb["on_file_received"](timestamp, file_path, original_name)

    def check_signatures(self, my_sign: str, companion_sign: str) -> bool:
        return bool(self._cb["check_signatures"](my_sign, companion_sign))

    def on_ready(self):
        self._cb["on_ready"]()

    def on_ping_timeout(self):
        self._cb["on_ping_timeout"]()

    def on_disconnect(self):
        self._cb["on_disconnect"]()


class CppModule(BaseModule):
    """BaseModule, чей транспорт — C++ TelegramClient (TDLib)."""

    unique_id = "zkgram.tdlib_1"
    name = "zkgram (TDLib)"
    description = "Транспорт поверх C++/TDLib слоя zkgram"
    supports_files = True

    # Реальные креды (API ID/Hash, телефон) настраиваются на стороне
    # C++/TDLib (см. src/config.hpp) — сюда идёт заглушка, т.к.
    # BaseModule требует непустой expected_credentials.
    expected_credentials = [Credential("noop", "Настраивается через src/config.hpp / TDLib")]

    class Sender(BaseModule.Sender):
        def __init__(self, credentials, user_id, callbacks: dict):
            super().__init__(credentials, user_id)
            self._cb = callbacks

        def send(self, text: str):
            self._cb["send_bytes"](text.encode("utf-8"))

        def send_file(self, file_path: str):
            self._cb["send_file"](file_path)

    class Listener(BaseModule.Listener):
        def __init__(self, credentials, ingester, file_ingester, user_id, callbacks: dict):
            super().__init__(credentials, ingester, file_ingester, user_id, BaseModule.stop_event)
            callbacks["register_receiver"](self._on_bytes, self._on_file)

        def _on_bytes(self, data: bytes):
            self.ingester(data.decode("utf-8"))

        def _on_file(self, file_path: str):
            self.file_ingester(file_path)

        def listen(self) -> str:
            pass

    def __init__(self, callbacks: dict):
        super().__init__()
        self._cb = callbacks

    def create_session(self, ingester, file_ingester=None):
        self.sender = self.Sender(self.credentials, self.user_id, self._cb)
        self.listener = self.Listener(self.credentials, ingester, file_ingester, self.user_id, self._cb)
