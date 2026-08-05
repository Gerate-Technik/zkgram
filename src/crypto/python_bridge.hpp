#pragma once

#include <pybind11/embed.h>

#include <string>

namespace py = pybind11;

// Даёт доступ к embedded Python-интерпретатору и знает про раскладку
// соседних Python-репозиториев CryptoLayer на диске. Единственный файл в
// src/crypto/, которому разрешено знать про это — crypto_layer.* работает
// только с результатом import().
//
// Сам интерпретатор ОДИН на процесс и живёт в python_bridge.cpp, а не в
// поле этого класса, хотя раньше было именно так. Причин две, обе
// приводили к неработающему шифрованному чату:
//
//  1) py::scoped_interpreter можно создать только один раз за процесс, а
//     CryptoLayer создаётся по одному на переписку (см. session.cpp). Со
//     вторым чатом конструктор второго интерпретатора бросил бы
//     исключение.
//  2) Уничтожение одного CryptoLayer финализировало бы Python целиком,
//     из-под ног всех остальных переписок.
//
// Этот класс теперь просто ручка: конструктор гарантирует, что
// интерпретатор поднят, и ничем не владеет.
namespace zkgram::crypto {

class PythonBridge {
public:
    PythonBridge();
    ~PythonBridge();

    PythonBridge(const PythonBridge&) = delete;
    PythonBridge& operator=(const PythonBridge&) = delete;

    // Вызывающая сторона обязана держать GIL (py::gil_scoped_acquire) —
    // см. docs/README.md §2. Интерпретатор GIL не удерживает, он отдан
    // сразу после инициализации.
    py::module_ import(const std::string& moduleName);
};

}  // namespace zkgram::crypto
