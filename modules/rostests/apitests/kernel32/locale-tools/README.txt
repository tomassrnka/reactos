Tools that produced the data of LocaleTable.c and of dll/win32/kernel32/winnls (Windows 10 22H2 is the reference)

ldump.c            dumps GetLocaleInfoEx, GetCalendarInfoEx, EnumCalendarInfoExEx, EnumDateFormatsExEx,
                   EnumTimeFormatsEx and the locale name functions, one line per value. Build it with the system
                   MinGW: x86_64-w64-mingw32-gcc -o ldump.exe ldump.c. ldump.exe [-N names.txt] [locale ...]
lcdiff.py          compares two dumps (python3 lcdiff.py windows.dump reactos.dump)
lcpatch.py         rewrites the LOCALE_* entries of winnls/nls/*.nls from a dump (python3 lcpatch.py windows.dump
                   winnls/nls [locale ...]; reads the LCTYPE names from sdk/include/psdk/winnls.h)
calgen.py          generates winnls/string/calendar_data.c from a dump
lctablegen.py      generates ../LocaleTable.h from a Windows dump and a ReactOS dump
build-standalone.sh builds LocaleTable.c to run on Windows
