# Phase-3c input: declared type | handle family | 32-to-64 | 64-to-32.
# Implements phase-3b design revision a48e73f1243. The HKL zero rule differs
# from both Windows reference measurements; see wow64handles.md before using
# this table as a Windows compatibility specification.
# Keep semantic types before resolving typedefs: HANDLE can also hide a GDI
# handle, a pointer, or an index. Such parameters need a per-call override.
# Unknown types must be reviewed, never classified by their H prefix.
set(WOW64_HANDLE_RULES
    "HWND|USER|SIGN|TRUNCATE"
    "HMENU|USER|SIGN|TRUNCATE"
    "HICON|USER|SIGN|TRUNCATE"
    "HCURSOR|USER|SIGN|TRUNCATE"
    "HHOOK|USER|SIGN|TRUNCATE"
    "HACCEL|USER|SIGN|TRUNCATE"
    "HMONITOR|USER|SIGN|TRUNCATE"
    "HIMC|USER|SIGN|TRUNCATE"
    "HWINEVENTHOOK|USER|SIGN|TRUNCATE"
    "HDWP|USER|SIGN|TRUNCATE"
    "HANDLE|NT|SIGN|TRUNCATE"
    "HDESK|NT|SIGN|TRUNCATE"
    "HWINSTA|NT|SIGN|TRUNCATE"
    "HDC|GDI|ZERO|TRUNCATE"
    "HBRUSH|GDI|ZERO|TRUNCATE"
    "HPEN|GDI|ZERO|TRUNCATE"
    "HFONT|GDI|ZERO|TRUNCATE"
    "HBITMAP|GDI|ZERO|TRUNCATE"
    "HRGN|GDI|ZERO|TRUNCATE"
    "HPALETTE|GDI|ZERO|TRUNCATE"
    "HGDIOBJ|GDI|ZERO|TRUNCATE"
    "HCOLORSPACE|GDI|ZERO|TRUNCATE"
    "HKL|LAYOUT|ZERO|TRUNCATE"
    "HINSTANCE|ADDRESS|ZERO|TRUNCATE"
    "HMODULE|ADDRESS|ZERO|TRUNCATE"
    "HGLOBAL|ADDRESS|ZERO|TRUNCATE"
    "HLOCAL|ADDRESS|ZERO|TRUNCATE")

# These carriers have integer semantics even when a message contains a handle.
# A message-specific thunk may use the handle table once it knows the payload.
set(WOW64_MESSAGE_INTEGER_RULES
    "LPARAM|INTEGER|SIGN|TRUNCATE"
    "WPARAM|INTEGER|ZERO|TRUNCATE"
    "LRESULT|INTEGER|SIGN|TRUNCATE")
