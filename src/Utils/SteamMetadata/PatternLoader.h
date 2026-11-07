#pragma once

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>

namespace PatternLoader {

    // Load metadata before installing hooks for a module.
    bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component);

    // Resolve by RVA first, then fall back to signature scanning.
    void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // Same table lookup without the missing-signature bookkeeping: a name that
    // is absent from the pattern file (or whose entry does not match this
    // build) yields nullptr and nothing else -- no WARN, no row in the
    // "Missing Signatures" popup. For optional targets whose absence is a
    // supported configuration rather than a broken signature.
    void* ResolveFromTable(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // Report unresolved functions after all hooks have been installed.
    void ReportMissingFunctions();

} // namespace PatternLoader
