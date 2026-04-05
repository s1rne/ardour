# SoundForge AI (патч поверх Ardour)

В эту ветку добавлено окно **SoundForge AI Assistant** (`gtk2_ardour/ai_window.cc`, `ai_window.h`):

- чат с локальным бэкендом (HTTP);
- кнопки Create/Apply MIDI-first проекта;
- локальный импорт MIDI в сессию через `PublicEditor::do_import` с дефолтным инструментом.

Меню: **Window → AI Assistant** (shortcut: Secondary+Tertiary+I, см. `ardour.keys.in`).

Сервер: отдельный репозиторий **soundforge-backend** (FastAPI на порту 8000 по умолчанию).

Лицензия исходного кода Ardour сохраняется (GPL v2+). Новые файлы помечены копирайтом SoundForge AI Team в шапках исходников.
