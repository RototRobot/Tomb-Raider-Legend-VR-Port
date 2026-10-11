#pragma once

namespace trlvr
{
    // The mod's own on-screen text in the game's language (2026-10-10).
    // The language is read from the game's string table (input_labels):
    // 0 English, 1 French, 2 German, 3 Italian, 4 Spanish. Text is keyed
    // by its English wording; anything not translated stays English.
    void lang_set(int language);
    int lang_current();
    // Changes whenever the language does (for cached text).
    unsigned lang_generation();
    // The text in the current language, UTF-8 / wide.
    const char* tr(const char* english);
    const wchar_t* trw(const wchar_t* english);
}
