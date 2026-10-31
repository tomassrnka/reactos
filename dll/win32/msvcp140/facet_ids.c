/*
 * PROJECT:     ReactOS msvcp140
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Data exports that no facet code of this DLL touches: std::locale::id of messages and
 *              time_get<unsigned short>, and the initialisation count of _UShinit
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Windows implements none of the messages facets in msvcp140.dll (the headers define them, like the
 * money facets), but it exports their std::locale::id objects so that every module shares one id per
 * facet. A program that calls std::use_facet<std::messages<char>> imports the id; without the export
 * it cannot be loaded. The values are those of msvcp140.dll 14.0.33728 on Windows 10 22H2, read
 * after load (all ids are 0 until a facet takes an id, _Init_cnt is -1).
 */

#include <stddef.h>

typedef struct {
    size_t id;
} locale_id;

locale_id messages_char_id = {0};
locale_id messages_short_id = {0};
locale_id messages_wchar_id = {0};
locale_id time_get_short_id = {0};

/* static int _UShinit::_Init_cnt: the unsigned short stream init class (_Winit is the wchar_t one) */
int _UShinit__Init_cnt = -1;
