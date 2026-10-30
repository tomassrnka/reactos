/*
 * PROJECT:     ReactOS msvcp140
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     std::locale::id objects of the money facets
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * The money facets (money_get, money_put, moneypunct) themselves are not implemented, as in Wine,
 * so std::locale does not hold them and use_facet on one of them throws std::bad_cast. Their ids
 * are exported all the same: programs built with Visual C++ import them (LibreOffice does), and
 * without them they cannot be loaded at all.
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
