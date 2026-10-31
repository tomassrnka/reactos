/*
 * PROJECT:     ReactOS msvcp140
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     std::locale::id objects and moneypunct::intl constants of the money facets
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * The money facets (money_get, money_put, moneypunct) are not part of msvcp140.dll: the Visual C++
 * headers define them as templates, and a program creates them itself when std::use_facet does not
 * find them in the locale (the inline _Getcat). Windows exports only the std::locale::id objects of
 * the facets, so that every module shares one id per facet, and the moneypunct::intl constants. The
 * default locale of Windows holds none of the money facets, and neither does the one of this DLL.
 * Programs built with Visual C++ import these symbols (LibreOffice does); without them they cannot
 * be loaded at all.
 */

#include <stddef.h>

typedef struct {
    size_t id;
} locale_id;

locale_id money_get_char_id = {0};
locale_id money_get_short_id = {0};
locale_id money_get_wchar_id = {0};
locale_id money_put_char_id = {0};
locale_id money_put_short_id = {0};
locale_id money_put_wchar_id = {0};
locale_id moneypunct_char_true_id = {0};
locale_id moneypunct_char_false_id = {0};
locale_id moneypunct_short_true_id = {0};
locale_id moneypunct_short_false_id = {0};
locale_id moneypunct_wchar_true_id = {0};
locale_id moneypunct_wchar_false_id = {0};

/* static const bool moneypunct<_Elem, _Intl>::intl, which is _Intl */
const unsigned char moneypunct_char_true_intl = 1;
const unsigned char moneypunct_char_false_intl = 0;
const unsigned char moneypunct_short_true_intl = 1;
const unsigned char moneypunct_short_false_intl = 0;
const unsigned char moneypunct_wchar_true_intl = 1;
const unsigned char moneypunct_wchar_false_intl = 0;
