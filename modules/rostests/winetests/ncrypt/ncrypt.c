/* Unit test suite for ncrypt.dll
 *
 * Copyright 2021 Santino Mazza
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <ncrypt.h>
#include <bcrypt.h>

#include "wine/test.h"

static UCHAR rsa_key_blob[] = {
    0x52, 0x53, 0x41, 0x31, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xc7, 0x8f, 0xac, 0x2a, 0xce, 0xbf, 0xc9, 0x6c, 0x7b,
    0x85, 0x74, 0x71, 0xbb, 0xff, 0xbb, 0x9b, 0x20, 0x03, 0x79, 0x17, 0x34,
    0xe7, 0x26, 0x91, 0x5c, 0x1f, 0x1b, 0x03, 0x3d, 0x46, 0xdf, 0xb6, 0xf2,
    0x10, 0x55, 0xf0, 0x39, 0x55, 0x0a, 0xe3, 0x9c, 0x0c, 0x63, 0xc2, 0x14,
    0x03, 0x94, 0x51, 0x0d, 0xb4, 0x22, 0x09, 0xf2, 0x5c, 0xb2, 0xd1, 0xc3,
    0xac, 0x6f, 0xa8, 0xc4, 0xac, 0xb8, 0xbc, 0x59, 0xe7, 0xed, 0x77, 0x6e,
    0xb1, 0x80, 0x58, 0x7d, 0xb2, 0x94, 0x46, 0xe5, 0x00, 0xe2, 0xb7, 0x33,
    0x48, 0x7a, 0xd3, 0x78, 0xe9, 0x26, 0x01, 0xc7, 0x00, 0x7b, 0x41, 0x6d,
    0x94, 0x3a, 0xe1, 0x50, 0x2b, 0x9f, 0x6b, 0x1c, 0x08, 0xa3, 0xfc, 0x0a,
    0x44, 0x81, 0x09, 0x41, 0x80, 0x23, 0x7b, 0xf6, 0x3f, 0xaf, 0x91, 0xa1,
    0x87, 0x75, 0x33, 0x15, 0xb8, 0xde, 0x32, 0x30, 0xb4, 0x5e, 0xfd};

static UCHAR rsa_private_key_blob[] = {
    0x52, 0x53, 0x41, 0x32, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xaf, 0xc0, 0x68, 0x21, 0x76, 0xd7, 0x46, 0xf8, 0x23,
    0x78, 0x94, 0x22, 0xa3, 0x0f, 0x4e, 0xe8, 0xc6, 0x6a, 0x33, 0xdc, 0xbf,
    0x44, 0x6c, 0xb6, 0x95, 0x17, 0x78, 0x1e, 0x75, 0xca, 0xdc, 0x66, 0xe4,
    0x87, 0xbe, 0x91, 0xbf, 0xb3, 0xc0, 0xa5, 0x16, 0xa1, 0xb7, 0xfe, 0x4c,
    0x67, 0xaa, 0x03, 0x38, 0xe7, 0x13, 0x65, 0xfb, 0xb6, 0x4c, 0x52, 0x29,
    0xa4, 0xc4, 0x92, 0x34, 0xcd, 0x81, 0xa5, 0x0b, 0x7f, 0x5f, 0x56, 0xbf,
    0xa7, 0xf8, 0xec, 0x9a, 0xe7, 0xb3, 0x93, 0xba, 0x00, 0xe9, 0x48, 0x7a,
    0xf7, 0x04, 0x65, 0xa3, 0x14, 0x8d, 0x08, 0x78, 0xd8, 0x16, 0x5e, 0x82,
    0xeb, 0xd8, 0xea, 0x3c, 0xec, 0xcc, 0x64, 0x47, 0x97, 0x69, 0x43, 0x17,
    0x5d, 0x25, 0xd9, 0xb8, 0xdf, 0xfc, 0x80, 0x16, 0xbe, 0xbb, 0xa8, 0xe4,
    0xbf, 0x6a, 0x2f, 0xea, 0x9c, 0xe5, 0x58, 0x8a, 0xf4, 0x2b, 0xe5, 0xcf,
    0x67, 0x26, 0xe2, 0xeb, 0x92, 0x88, 0xde, 0x52, 0xee, 0x43, 0x44, 0x14,
    0x6d, 0xf7, 0x46, 0x9c, 0x87, 0xa8, 0x96, 0x85, 0xda, 0x19, 0xc3, 0x57,
    0x95, 0x09, 0x8d, 0xa2, 0x4e, 0xcd, 0x84, 0x67, 0x8e, 0x2c, 0x2d, 0x16,
    0xb0, 0xb4, 0xb4, 0x66, 0xcc, 0x7b, 0x7f, 0xaa, 0x2b, 0x14, 0x17, 0x4c,
    0x68, 0x4a, 0xa2, 0xd3, 0xfc, 0xe0, 0xb9, 0xcd, 0xa4, 0xbb, 0x44, 0x0b,
    0x84, 0xa7, 0x13, 0xd8, 0xee, 0xae, 0x8e, 0xdb, 0x0b, 0xd3, 0xc5, 0xc5,
    0x3b, 0xfd, 0x6c, 0xb0, 0x92, 0x14, 0xf0, 0x7b, 0xa7, 0x37, 0xd6, 0x21,
    0x73, 0x5d, 0x80, 0x9a, 0x49, 0x71, 0x2f, 0xc9, 0x05, 0xa5, 0x74, 0x81,
    0xc9, 0xd5, 0x2a, 0x6a, 0x7f, 0xe8, 0x8e, 0xa9, 0x7b, 0x79, 0x9e, 0x46,
    0x51, 0xe7, 0x08, 0xa1, 0x75, 0x51, 0x75, 0x7a, 0x4a, 0x43, 0x0e, 0x5b,
    0x5a, 0xb8, 0x1b, 0xa3, 0x85, 0x68, 0x27};

static UCHAR rsa_full_private_key_blob[] = {
    0x52, 0x53, 0x41, 0x33, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xbb, 0x49, 0x0d, 0x3f, 0xfa, 0x15, 0x1a, 0xd7, 0xd4,
    0x7e, 0x51, 0xe7, 0x7f, 0xaf, 0xbb, 0x63, 0x8e, 0x88, 0x71, 0x8c, 0x67,
    0x91, 0xb0, 0x6b, 0xd2, 0x4f, 0x58, 0xb4, 0xd7, 0xdd, 0xcb, 0xa8, 0xc0,
    0x0c, 0xf5, 0x41, 0xfd, 0x28, 0xdf, 0xb9, 0x0d, 0x4e, 0x22, 0x24, 0x37,
    0xa5, 0xae, 0x3c, 0x9a, 0xbf, 0xae, 0x8f, 0xd9, 0xc1, 0x30, 0x87, 0x71,
    0xee, 0x5a, 0x2b, 0xfc, 0x2d, 0x5b, 0xf6, 0xe7, 0xf1, 0x69, 0xf2, 0x94,
    0x47, 0x55, 0x6e, 0xca, 0x4f, 0x66, 0xba, 0x82, 0xbb, 0x98, 0x09, 0xd0,
    0x3b, 0x8e, 0x30, 0x20, 0x0e, 0x8f, 0x44, 0xc8, 0x8d, 0xfd, 0x06, 0x65,
    0x6d, 0x7a, 0xbb, 0x0d, 0xad, 0x5c, 0x3f, 0xc5, 0x7c, 0x57, 0x0a, 0x3b,
    0x21, 0x54, 0x3e, 0xd1, 0x54, 0x07, 0x40, 0xbb, 0x85, 0x78, 0x55, 0x47,
    0x2a, 0x06, 0xbd, 0x81, 0xa7, 0x77, 0x32, 0x37, 0x56, 0x3c, 0x49, 0xf2,
    0x68, 0x32, 0x44, 0x8c, 0x6a, 0x0b, 0xfa, 0xa4, 0x66, 0xc9, 0xcc, 0x54,
    0xab, 0xb3, 0x90, 0x65, 0xce, 0x43, 0xba, 0xcd, 0xc1, 0x6a, 0x8a, 0x1b,
    0xdc, 0x16, 0x8f, 0xea, 0xf0, 0x25, 0x74, 0x6d, 0xc4, 0x84, 0x73, 0xc7,
    0xc9, 0x38, 0xa9, 0x59, 0x5b, 0xaf, 0x59, 0x9e, 0x2f, 0x56, 0x75, 0x30,
    0xc4, 0x5e, 0x80, 0x9b, 0x0b, 0x80, 0xc2, 0x5d, 0x2c, 0xb1, 0xd7, 0x65,
    0xd6, 0x7f, 0x2b, 0xc5, 0xc9, 0x92, 0x89, 0xa4, 0x83, 0x7c, 0xb2, 0x48,
    0xa2, 0x25, 0x4e, 0x8a, 0x59, 0x91, 0xbc, 0x53, 0x88, 0xa8, 0x51, 0x5d,
    0x19, 0xed, 0x5d, 0x5e, 0xde, 0x42, 0x09, 0xd7, 0x41, 0x1a, 0x3b, 0xa5,
    0xfc, 0x51, 0x7b, 0x93, 0x1e, 0x11, 0x18, 0xa0, 0x64, 0x89, 0xd5, 0x91,
    0xdf, 0x79, 0xed, 0x89, 0x49, 0x5c, 0x7d, 0x5f, 0x85, 0x54, 0x7c, 0x4d,
    0xcb, 0x8f, 0x0e, 0xbb, 0x01, 0x18, 0x5b, 0x46, 0x89, 0x27, 0x47, 0x67,
    0x0c, 0x5c, 0x9b, 0xfc, 0x46, 0xa4, 0xa5, 0xbe, 0x65, 0x90, 0x04, 0x8e,
    0x7b, 0x2c, 0x9e, 0x28, 0xcd, 0x05, 0x31, 0x56, 0xd5, 0xfe, 0x02, 0xb6,
    0xbc, 0x56, 0x49, 0xc0, 0xbf, 0x14, 0x43, 0x94, 0x78, 0xea, 0xf1, 0xec,
    0x3c, 0x8f, 0x93, 0xa6, 0x41, 0xe4, 0x08, 0x89, 0xbe, 0x3a, 0x39, 0x75,
    0xe4, 0x2c, 0xfd, 0x61, 0x9b, 0x65, 0xcc, 0x65, 0xa7, 0xba, 0xad, 0x15,
    0x46, 0xcb, 0xd6, 0xb6, 0xb5, 0xd6, 0x52, 0x2f, 0x72, 0x11, 0xe5, 0x53,
    0x08, 0xb3, 0x9e, 0xb8, 0xb5, 0xb8, 0xb5, 0x97, 0xf8, 0x54, 0x0a, 0x79,
    0x4c, 0x1b, 0x7a, 0x87, 0x87, 0x5f, 0x55, 0x0f, 0x54, 0x07, 0x47, 0xed,
    0xd9, 0x8c, 0x29, 0x44, 0x07, 0xcc, 0xb4, 0x06, 0xcf, 0x2d, 0x84, 0x9a,
    0x02, 0x24, 0x4a, 0xf0, 0x5d, 0x9d, 0x6b, 0x09, 0xcb, 0x5e, 0x06, 0x4d,
    0xa3, 0x9e, 0x85, 0x38, 0x89, 0xf4, 0x44, 0xbd, 0x4a, 0xe3, 0x2e, 0x3c,
    0x74, 0x5c, 0xad, 0xc3, 0x17, 0x6b, 0x84, 0x24, 0x94, 0xcd, 0xa1, 0x21,
    0xec, 0x0a, 0x4c, 0x66, 0x38, 0x95, 0xc1, 0x64, 0x78, 0xf5, 0x12, 0x46,
    0x58, 0x7f, 0x8a, 0x3f, 0xce, 0xda, 0x9b, 0x7e, 0xa5, 0xe4, 0xbe, 0x9c,
    0x15, 0xd4, 0xb0, 0x84, 0xf0, 0xed, 0x94, 0x37, 0x71, 0x41, 0xbe, 0x9a,
    0x02, 0x02, 0xd9, 0xc2, 0xaf, 0x44, 0x32, 0x6d, 0xcd, 0xee, 0xce, 0xcb,
    0xb9, 0x03, 0xfb, 0xf6, 0xc1, 0xf9, 0x13, 0x59, 0x71, 0x96, 0xd4, 0x50,
    0x04, 0xff, 0x4d, 0xea, 0x8b, 0x28, 0x7c, 0x8e, 0xd9, 0xa6, 0x90, 0xa0,
    0xdf, 0x91, 0x40, 0x67, 0x19, 0x31, 0xd2, 0x76, 0xb3, 0x38, 0x38, 0x2e,
    0xec, 0xd8, 0x67, 0x93, 0x98, 0xb8, 0x07, 0x20, 0x53, 0xb1, 0xec, 0xf5,
    0x26, 0xa7, 0x65, 0x42, 0x0f, 0x15, 0x1f, 0x24, 0xc3, 0x13, 0x5f, 0x35,
    0x97, 0x8f, 0x23, 0x3e, 0x7c, 0xaf, 0xe8, 0x19, 0xeb, 0xee, 0xc6, 0xd2,
    0x1d, 0x5b, 0x75, 0x31, 0xae, 0xea, 0xc1, 0x5a, 0x49, 0xa4, 0xf4, 0x37,
    0x65, 0x16, 0xc9, 0xfb, 0x0a, 0x9d, 0x33, 0x5a, 0x44, 0xaa, 0x9b, 0x0b,
    0x62, 0x48, 0x5f, 0x4c, 0xb5, 0xba, 0x3f, 0xf7, 0xb5, 0xb4, 0xed, 0x84,
    0xbe, 0xc7, 0x6c, 0x1f, 0xa4, 0xd1, 0xb8, 0x23, 0xf6, 0xa6, 0x47, 0x06,
    0x09, 0x4a, 0x61};

static UCHAR rsa_key_blob_with_invalid_bit_length[] = {
    0x52, 0x53, 0x41, 0x31, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xc7, 0x8f, 0xac, 0x2a, 0xce, 0xbf, 0xc9, 0x6c, 0x7b,
    0x85, 0x74, 0x71, 0xbb, 0xff, 0xbb, 0x9b, 0x20, 0x03, 0x79, 0x17, 0x34,
    0xe7, 0x26, 0x91, 0x5c, 0x1f, 0x1b, 0x03, 0x3d, 0x46, 0xdf, 0xb6, 0xf2,
    0x10, 0x55, 0xf0, 0x39, 0x55, 0x0a, 0xe3, 0x9c, 0x0c, 0x63, 0xc2, 0x14,
    0x03, 0x94, 0x51, 0x0d, 0xb4, 0x22, 0x09, 0xf2, 0x5c, 0xb2, 0xd1, 0xc3,
    0xac, 0x6f, 0xa8, 0xc4, 0xac, 0xb8, 0xbc, 0x59, 0xe7, 0xed, 0x77, 0x6e,
    0xb1, 0x80, 0x58, 0x7d, 0xb2, 0x94, 0x46, 0xe5, 0x00, 0xe2, 0xb7, 0x33,
    0x48, 0x7a, 0xd3, 0x78, 0xe9, 0x26, 0x01, 0xc7, 0x00, 0x7b, 0x41, 0x6d,
    0x94, 0x3a, 0xe1, 0x50, 0x2b, 0x9f, 0x6b, 0x1c, 0x08, 0xa3, 0xfc, 0x0a,
    0x44, 0x81, 0x09, 0x41, 0x80, 0x23, 0x7b, 0xf6, 0x3f, 0xaf, 0x91, 0xa1,
    0x87, 0x75, 0x33, 0x15, 0xb8, 0xde, 0x32, 0x30, 0xb4, 0x5e, 0xfd};

static UCHAR rsa_key_blob_with_invalid_publicexp_size[] = {
    0x52, 0x53, 0x41, 0x31, 0x00, 0x04, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xc7, 0x8f, 0xac, 0x2a, 0xce, 0xbf, 0xc9, 0x6c, 0x7b,
    0x85, 0x74, 0x71, 0xbb, 0xff, 0xbb, 0x9b, 0x20, 0x03, 0x79, 0x17, 0x34,
    0xe7, 0x26, 0x91, 0x5c, 0x1f, 0x1b, 0x03, 0x3d, 0x46, 0xdf, 0xb6, 0xf2,
    0x10, 0x55, 0xf0, 0x39, 0x55, 0x0a, 0xe3, 0x9c, 0x0c, 0x63, 0xc2, 0x14,
    0x03, 0x94, 0x51, 0x0d, 0xb4, 0x22, 0x09, 0xf2, 0x5c, 0xb2, 0xd1, 0xc3,
    0xac, 0x6f, 0xa8, 0xc4, 0xac, 0xb8, 0xbc, 0x59, 0xe7, 0xed, 0x77, 0x6e,
    0xb1, 0x80, 0x58, 0x7d, 0xb2, 0x94, 0x46, 0xe5, 0x00, 0xe2, 0xb7, 0x33,
    0x48, 0x7a, 0xd3, 0x78, 0xe9, 0x26, 0x01, 0xc7, 0x00, 0x7b, 0x41, 0x6d,
    0x94, 0x3a, 0xe1, 0x50, 0x2b, 0x9f, 0x6b, 0x1c, 0x08, 0xa3, 0xfc, 0x0a,
    0x44, 0x81, 0x09, 0x41, 0x80, 0x23, 0x7b, 0xf6, 0x3f, 0xaf, 0x91, 0xa1,
    0x87, 0x75, 0x33, 0x15, 0xb8, 0xde, 0x32, 0x30, 0xb4, 0x5e, 0xfd};

/* RSA public key with invalid magic value. */
static UCHAR invalid_rsa_key_blob[] = {
    0x75, 0x59, 0x41, 0x31, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xc7, 0x8f, 0xac, 0x2a, 0xce, 0xbf, 0xc9, 0x6c, 0x7b,
    0x85, 0x74, 0x71, 0xbb, 0xff, 0xbb, 0x9b, 0x20, 0x03, 0x79, 0x17, 0x34,
    0xe7, 0x26, 0x91, 0x5c, 0x1f, 0x1b, 0x03, 0x3d, 0x46, 0xdf, 0xb6, 0xf2,
    0x10, 0x55, 0xf0, 0x39, 0x55, 0x0a, 0xe3, 0x9c, 0x0c, 0x63, 0xc2, 0x14,
    0x03, 0x94, 0x51, 0x0d, 0xb4, 0x22, 0x09, 0xf2, 0x5c, 0xb2, 0xd1, 0xc3,
    0xac, 0x6f, 0xa8, 0xc4, 0xac, 0xb8, 0xbc, 0x59, 0xe7, 0xed, 0x77, 0x6e,
    0xb1, 0x80, 0x58, 0x7d, 0xb2, 0x94, 0x46, 0xe5, 0x00, 0xe2, 0xb7, 0x33,
    0x48, 0x7a, 0xd3, 0x78, 0xe9, 0x26, 0x01, 0xc7, 0x00, 0x7b, 0x41, 0x6d,
    0x94, 0x3a, 0xe1, 0x50, 0x2b, 0x9f, 0x6b, 0x1c, 0x08, 0xa3, 0xfc, 0x0a,
    0x44, 0x81, 0x09, 0x41, 0x80, 0x23, 0x7b, 0xf6, 0x3f, 0xaf, 0x91, 0xa1,
    0x87, 0x75, 0x33, 0x15, 0xb8, 0xde, 0x32, 0x30, 0xb4, 0x5e, 0xfd};

struct expected_property
{
    const WCHAR *name;
    BYTE expected_data[64];
    DWORD expected_size;
    SECURITY_STATUS expected_status;
    BOOL todo;
    SECURITY_STATUS broken_status;
    BYTE broken_data[64];
};

static void check_property(int line, NCRYPT_KEY_HANDLE key, const struct expected_property *property)
{
    BYTE buffer[64] = {0};
    SECURITY_STATUS ret;
    unsigned int i;
    DWORD size;

    winetest_push_context("%s", wine_dbgstr_w(property->name));

    if (property->expected_status != ERROR_SUCCESS)
    {
        size = 0;
        ret = NCryptGetProperty(key, property->name, buffer, sizeof(buffer), &size, 0);
        todo_wine_if(property->todo)
        ok_(__FILE__, line)(ret == property->expected_status || broken(ret == property->broken_status),
                            "got unexpected return value %#lx\n", ret);
        if (ret == ERROR_SUCCESS)
        {
            ok_(__FILE__, line)(size == property->expected_size, "got unexpected size %lu\n", size);

            if (!wcscmp(property->name, NCRYPT_PROVIDER_HANDLE_PROPERTY))
                NCryptFreeObject((NCRYPT_HANDLE)buffer);
        }

        goto done;
    }

    size = 0;
    ret = NCryptGetProperty(key, property->name, NULL, 0, &size, 0);
    todo_wine_if(property->todo)
    ok_(__FILE__, line)(ret == property->expected_status || broken(ret == property->broken_status),
                        "got unexpected return value %#lx\n", ret);
    if (ret != ERROR_SUCCESS)
        goto done;
    ok_(__FILE__, line)(size == property->expected_size, "got unexpected size %lu\n", size);

    size = 0;
    ret = NCryptGetProperty(key, property->name, buffer, sizeof(buffer), &size, 0);
    ok_(__FILE__, line)(ret == property->expected_status, "got unexpected return value %#lx\n", ret);
    ok_(__FILE__, line)(size == property->expected_size, "got unexpected size %lu\n", size);
    if (ret == ERROR_SUCCESS && memcmp(property->expected_data, buffer, property->expected_size)
        && broken(memcmp(property->broken_data, buffer, property->expected_size)))
    {
        for (i = 0; i < size; i ++)
        {
            ok_(__FILE__, line)(0, "%#04x \n", buffer[i]);
        }
    }

done:
    winetest_pop_context();
}

#define check_properties(a, b) _check_properties(__LINE__, a, b)
static void _check_properties(int line, NCRYPT_PROV_HANDLE prov, NCRYPT_KEY_HANDLE key)
{
    struct expected_property properties[] =
    {
        /* NCrypt properties */
        {NCRYPT_NAME_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_UNIQUE_NAME_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_ALGORITHM_PROPERTY, {'R', 0, 'S', 0, 'A', 0}, sizeof(L"RSA"), ERROR_SUCCESS},
        {NCRYPT_LENGTH_PROPERTY, {0, 0x4} /* 1024 */, sizeof(DWORD)},
        {NCRYPT_LENGTHS_PROPERTY, {0, 0x2, 0, 0, 0, 0x40, 0, 0, 0x8, 0, 0, 0, 0, 0x4, 0, 0}, sizeof(DWORD) * 4 /* NCRYPT_SUPPORTED_LENGTHS */, ERROR_SUCCESS, FALSE, ERROR_SUCCESS, {0, 0x2, 0, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0, 0, 0x4, 0, 0}},
        {NCRYPT_BLOCK_LENGTH_PROPERTY, {0x80, 0, 0, 0}, sizeof(DWORD), ERROR_SUCCESS},
        {NCRYPT_UI_POLICY_PROPERTY, {0}, sizeof(NCRYPT_UI_POLICY), NTE_NOT_FOUND, FALSE},
        {NCRYPT_EXPORT_POLICY_PROPERTY, {0}, sizeof(DWORD), ERROR_SUCCESS},
        {NCRYPT_WINDOW_HANDLE_PROPERTY, {0}, 0, NTE_NOT_FOUND, FALSE},
        {NCRYPT_USE_CONTEXT_PROPERTY, {0}, 0, NTE_NOT_FOUND, FALSE},
        {NCRYPT_IMPL_TYPE_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_KEY_USAGE_PROPERTY, {0xff, 0xff, 0xff, 0} /* NCRYPT_ALLOW_ALL_USAGES */, sizeof(DWORD), ERROR_SUCCESS},
        {NCRYPT_KEY_TYPE_PROPERTY, {0}, sizeof(DWORD), ERROR_SUCCESS},
        {NCRYPT_VERSION_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_SECURITY_DESCR_SUPPORT_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_SECURITY_DESCR_PROPERTY, {0}, 0, NTE_BAD_FLAGS, FALSE},
        {NCRYPT_USE_COUNT_ENABLED_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_USE_COUNT_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_LAST_MODIFIED_PROPERTY, {0}, sizeof(FILETIME), NTE_NOT_FOUND, FALSE},
        {NCRYPT_WINDOW_HANDLE_PROPERTY, {0}, 0, NTE_NOT_FOUND, FALSE},
        {NCRYPT_MAX_NAME_LENGTH_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_ALGORITHM_GROUP_PROPERTY, {'R', 0, 'S', 0, 'A', 0}, sizeof(L"RSA")},
        {NCRYPT_PROVIDER_HANDLE_PROPERTY, {0}, sizeof(NCRYPT_PROV_HANDLE), ERROR_SUCCESS},
        {NCRYPT_PIN_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_READER_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_SMARTCARD_GUID_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_CERTIFICATE_PROPERTY, {0}, 0, NTE_NOT_FOUND, FALSE, NTE_NOT_SUPPORTED /* Win 7 */},
        {NCRYPT_PIN_PROMPT_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_USER_CERTSTORE_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_ROOT_CERTSTORE_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_SECURE_PIN_PROPERTY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_ASSOCIATED_ECDH_KEY, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_SCARD_PIN_ID, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {NCRYPT_SCARD_PIN_INFO, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        /* BCrypt properties */
        {BCRYPT_ALGORITHM_NAME, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_AUTH_TAG_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_BLOCK_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_BLOCK_SIZE_LIST, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_CHAINING_MODE, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_EFFECTIVE_KEY_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_HASH_BLOCK_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_HASH_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_HASH_OID_LIST, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_KEY_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_KEY_LENGTHS, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_KEY_OBJECT_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_KEY_STRENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_OBJECT_LENGTH, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_PADDING_SCHEMES, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_PROVIDER_HANDLE, {0}, 0, NTE_NOT_SUPPORTED, FALSE},
        {BCRYPT_SIGNATURE_LENGTH, {0x80, 0, 0, 0} /* 128 */, sizeof(DWORD), ERROR_SUCCESS, FALSE, NTE_NOT_SUPPORTED /* <= Win 8 */},
        {BCRYPT_PUBLIC_KEY_LENGTH, {0, 0x4} /* 1024 */, sizeof(DWORD), ERROR_SUCCESS, FALSE, NTE_NOT_SUPPORTED /* <= Win 8 */},
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(properties); i++)
    {
        if (!lstrcmpW(properties[i].name, NCRYPT_PROVIDER_HANDLE_PROPERTY))
            memcpy(properties[i].expected_data, &prov, sizeof(prov));

        check_property(line, key, &properties[i]);
    }
}

static void test_key_import_rsa(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;

    prov = 0;
    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(prov, "got null handle\n");

    key = 0;
    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPUBLIC_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(key, "got null handle\n");
    NCryptFreeObject(key);

    key = 0;
    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(key, "got null handle\n");
    NCryptFreeObject(key);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob), 49);
    ok(ret == NTE_BAD_FLAGS, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, invalid_rsa_key_blob,
                          sizeof(invalid_rsa_key_blob), 0);
    ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, NULL, 0, 0);
    ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);

    key = 0;
    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, rsa_key_blob_with_invalid_bit_length,
                          sizeof(rsa_key_blob_with_invalid_bit_length), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret); /* I'm not sure why, but this returns success */
    ok(key, "got null handle\n");
    NCryptFreeObject(key);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, rsa_key_blob_with_invalid_publicexp_size,
                          sizeof(rsa_key_blob_with_invalid_publicexp_size), 0);
    ok(ret == NTE_BAD_DATA, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob) - 1, 0);
    ok(ret == NTE_BAD_DATA, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob) + 1, 0);
    ok(ret == NTE_BAD_DATA, "got %#lx\n", ret);

    NCryptFreeObject(prov);
}

static void test_free_object(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    char *buf;

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPUBLIC_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptFreeObject(key);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    key = 0;
    ret = NCryptFreeObject(key);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    buf = calloc(1, 50);
    ret = NCryptFreeObject((NCRYPT_KEY_HANDLE)buf);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);
    free(buf);

    NCryptFreeObject(prov);
}

static void test_get_property(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    DWORD keylength, size;

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptGetProperty(0, NCRYPT_LENGTH_PROPERTY, (BYTE *)&keylength, sizeof(keylength), &size, 0);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPUBLIC_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_properties(prov, key);
    NCryptFreeObject(key);

    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, &key, rsa_private_key_blob, sizeof(rsa_private_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_properties(prov, key);
    NCryptFreeObject(key);

    ret = NCryptImportKey(prov, 0, BCRYPT_RSAFULLPRIVATE_BLOB, NULL, &key, rsa_full_private_key_blob, sizeof(rsa_full_private_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_properties(prov, key);
    NCryptFreeObject(key);

    NCryptFreeObject(prov);
}

static void test_set_property(void)
{
    NCRYPT_PROV_HANDLE prov;
    SECURITY_STATUS ret;
    NCRYPT_KEY_HANDLE key;
    DWORD keylength;

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPUBLIC_BLOB, NULL, &key, rsa_key_blob, sizeof(rsa_key_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    keylength = 2048;
    ret = NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, (BYTE *)&keylength, sizeof(keylength), 0);
    ok(ret == ERROR_SUCCESS || broken(ret == NTE_INVALID_HANDLE), "got %#lx\n", ret);

    ret = NCryptSetProperty(0, NCRYPT_LENGTH_PROPERTY, (BYTE *)&keylength, sizeof(keylength), 0);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    ret = NCryptSetProperty(key, NCRYPT_NAME_PROPERTY, (BYTE *)L"Key name", sizeof(L"Key name"), 0);
    ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);
    NCryptFreeObject(key);

    key = 0;
    ret = NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(key, "got null handle\n");

    keylength = 2048;
    ret = NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, (BYTE *)&keylength, sizeof(keylength), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptSetProperty(key, NCRYPT_PROVIDER_HANDLE_PROPERTY, (BYTE *)&prov, sizeof(prov), 0);
    ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);

    ret = NCryptSetProperty(key, NCRYPT_NAME_PROPERTY, (BYTE *)L"Key name", sizeof(L"Key name"), 0);
    ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);

    ret = NCryptSetProperty(key, L"My Custom Property", (BYTE *)L"value", sizeof(L"value"), 0);
    ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);
    NCryptFreeObject(key);
    NCryptFreeObject(prov);
}

static void test_create_persisted_key(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    key = 0;
    ret = NCryptCreatePersistedKey(0, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    ret = NCryptCreatePersistedKey(prov, &key, NULL, NULL, 0, 0);
    ok(ret == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER) || broken(ret == NTE_FAIL), "got %#lx\n", ret);

    ret = NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(key, "got null handle\n");

    check_properties(prov, key);

    NCryptFinalizeKey(key, 0);
    NCryptFreeObject(key);

    todo_wine
    {
    key = 0;
    ret = NCryptCreatePersistedKey(prov, &key, BCRYPT_AES_ALGORITHM, NULL, 0, 0);
    ok(ret == ERROR_SUCCESS || broken(ret == NTE_NOT_SUPPORTED) /* win 7 */, "got %#lx\n", ret);
    if (ret == NTE_NOT_SUPPORTED) win_skip("broken, symmetric keys not supported.\n");
    else ok(key, "got null handle\n");
    }

    NCryptFreeObject(prov);
}

static void test_finalize_key(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptFinalizeKey(key, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptFinalizeKey(key, 0);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    ret = NCryptFinalizeKey(0, 0);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    NCryptFreeObject(key);
}

static UCHAR signature_pubkey_blob[] = {
  0x52, 0x53, 0x41, 0x31, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x01, 0xb4, 0xdb, 0x15, 0x24, 0xd0, 0x9a, 0xf4, 0xa3, 0x22,
  0x04, 0xbe, 0xbd, 0xda, 0xb4, 0x3b, 0x1f, 0x9d, 0x5f, 0x2a, 0x3e, 0xe7,
  0x42, 0xd7, 0xfa, 0xbc, 0xeb, 0x4e, 0xba, 0xcf, 0x92, 0x5d, 0x42, 0x0e,
  0xc0, 0xb0, 0x0f, 0xcc, 0xc3, 0x03, 0x69, 0x23, 0xb9, 0xc2, 0x07, 0xf7,
  0xf5, 0x19, 0xef, 0x9f, 0xb6, 0xd4, 0x6d, 0x35, 0x0d, 0x4c, 0x3a, 0x2b,
  0x37, 0x9d, 0x4d, 0x61, 0xce, 0x30, 0x33, 0x27, 0xe4, 0x9a, 0xca, 0x23,
  0xd6, 0x8f, 0x90, 0x33, 0xba, 0x5b, 0x16, 0xab, 0xa6, 0xda, 0x7a, 0x16,
  0x8e, 0xae, 0xf9, 0x6f, 0x29, 0xe2, 0xf1, 0x3d, 0xf0, 0xb6, 0xb4, 0x70,
  0xc0, 0x6d, 0xfc, 0x73, 0xf1, 0x3e, 0xdf, 0x02, 0x2c, 0xab, 0x24, 0x7a,
  0xd8, 0x8b, 0xcf, 0x63, 0xd0, 0x1a, 0x76, 0xb3, 0x11, 0x22, 0x5d, 0xf6,
  0x72, 0xb5, 0xd1, 0x6b, 0x83, 0xcd, 0x63, 0x5d, 0x3d, 0x03, 0xa2, 0xe0,
  0x36, 0xe8, 0x54, 0xe9, 0x6f, 0x04, 0xb3, 0x1a, 0xdf, 0x35, 0xd7, 0x01,
  0x57, 0xc7, 0x8d, 0x81, 0x4b, 0x39, 0x0c, 0x92, 0x71, 0x0b, 0x7d, 0x05,
  0xb4, 0xa4, 0xb4, 0xd4, 0x73, 0x55, 0x85, 0x19, 0xd5, 0x37, 0x7f, 0xcd,
  0x81, 0xd3, 0xf2, 0x62, 0x60, 0xfb, 0x6a, 0x1a, 0xda, 0xb4, 0x83, 0xfa,
  0x87, 0xac, 0x51, 0x3a, 0xb4, 0xc2, 0x03, 0xd3, 0xc6, 0xf0, 0x20, 0xca,
  0x10, 0xfd, 0xeb, 0xd8, 0x85, 0xb9, 0x74, 0x29, 0x83, 0xda, 0xa2, 0xee,
  0xcc, 0xf4, 0xf6, 0x18, 0xbc, 0x84, 0x92, 0xf5, 0xdb, 0x71, 0x14, 0xb9,
  0x62, 0x54, 0x4b, 0x64, 0x58, 0x95, 0xbc, 0x69, 0xc1, 0xcd, 0xac, 0x4b,
  0x7c, 0x1d, 0xc4, 0xe7, 0x6c, 0x44, 0x80, 0x5e, 0x88, 0x3a, 0x9c, 0xbd,
  0xd5, 0xa5, 0x2f, 0xa6, 0x73, 0x65, 0x48, 0x00, 0x5e, 0xfd, 0xe9, 0x4e,
  0xd7, 0x6c, 0x59, 0x47, 0x9b, 0xd1, 0x9d};

static UCHAR signature_pkcs1_sha256[] = {
  0xad, 0x3d, 0x2f, 0x31, 0x87, 0xba, 0x5b, 0xdc, 0x84, 0x95, 0x77, 0x68,
  0xf9, 0x9c, 0xd2, 0x38, 0x5b, 0x9f, 0x09, 0xc3, 0x8e, 0x9e, 0x8e, 0x8d,
  0x49, 0x56, 0x31, 0x09, 0x55, 0xc6, 0xd2, 0x1a, 0xb3, 0x00, 0x71, 0x60,
  0x84, 0x38, 0xf7, 0x1b, 0xc0, 0x43, 0x87, 0x31, 0x2d, 0xcc, 0xf2, 0x49,
  0x5f, 0x21, 0x8e, 0x77, 0x16, 0x57, 0xd7, 0xe9, 0x37, 0x3a, 0xe9, 0x91,
  0x5b, 0xc5, 0xea, 0x34, 0xb5, 0xf7, 0x4d, 0xaa, 0x65, 0x06, 0xef, 0xc6,
  0x14, 0xfa, 0xa7, 0x1e, 0xe5, 0xc6, 0x18, 0xfe, 0x06, 0x64, 0xe7, 0x6e,
  0x11, 0x83, 0x03, 0x4c, 0x91, 0x47, 0x60, 0x88, 0x12, 0x0c, 0x5e, 0xcc,
  0x3a, 0x93, 0x2f, 0x5c, 0x57, 0x5b, 0x66, 0x55, 0xa3, 0xe5, 0xf4, 0x3c,
  0x6f, 0xe9, 0xd8, 0xfd, 0xcc, 0x19, 0xc8, 0xf8, 0x13, 0x21, 0x3d, 0x29,
  0xee, 0x3c, 0xb2, 0xb4, 0x3f, 0x36, 0xd8, 0x82, 0xb1, 0xa9, 0x34, 0x89,
  0x2a, 0x7a, 0x42, 0x4a, 0x13, 0x0a, 0x0b, 0x50, 0xbe, 0x43, 0xa8, 0x2c,
  0x5a, 0x86, 0x65, 0x9f, 0xab, 0x86, 0x25, 0x44, 0xbb, 0x6b, 0xca, 0x04,
  0xca, 0xa5, 0xe7, 0x8e, 0xe2, 0x1d, 0xa8, 0x31, 0x90, 0x85, 0x6a, 0xd9,
  0xaf, 0x67, 0x8c, 0x1a, 0x13, 0xea, 0xa8, 0xfd, 0x3d, 0xc0, 0xb0, 0x3f,
  0xde, 0x6a, 0xc9, 0x65, 0xeb, 0x0a, 0xa6, 0x9b, 0xfc, 0x97, 0x0a, 0x72,
  0xaf, 0x04, 0xde, 0x3f, 0xcb, 0xac, 0x97, 0x6e, 0xfa, 0xbb, 0xd8, 0x6e,
  0xad, 0x0d, 0x10, 0xc6, 0x34, 0x18, 0x50, 0xb9, 0x12, 0xd4, 0x6b, 0xd5,
  0x3d, 0x9d, 0xfb, 0x31, 0x8d, 0x96, 0x39, 0x4d, 0x20, 0x91, 0x40, 0x06,
  0xca, 0xe6, 0x33, 0x73, 0x76, 0x2a, 0xe3, 0xab, 0xaf, 0x9d, 0xe9, 0x4e,
  0xb7, 0xe0, 0x4b, 0x45, 0x23, 0x07, 0xe9, 0xe2, 0xab, 0xdc, 0x9f, 0x5a,
  0xed, 0xbe, 0x76, 0x27};

static UCHAR invalid_signature[] = {
  0x4f, 0x8f, 0x88, 0x43, 0xed, 0x9e, 0xe8, 0x75, 0x0f, 0xe7, 0x32, 0xab,
  0x07, 0xcc, 0x6c, 0x8d, 0x56, 0xaa, 0x78, 0xf8, 0x43, 0x62, 0x4c, 0x0c,
  0xd5, 0x45, 0x2c, 0x3d, 0x28, 0x6f, 0xbc, 0x8b, 0xc1, 0x9f, 0xfe, 0x33,
  0x50, 0x4e, 0xd0, 0x6a, 0x83, 0xf8, 0x0e, 0xc8, 0x19, 0x59, 0xde, 0x25,
  0xf0, 0x7b, 0x4e, 0x3d, 0x9d, 0xc7, 0xbf, 0x72, 0xc3, 0x26, 0x48, 0xb6,
  0x19, 0x08, 0x3f, 0x05, 0xe8, 0x76, 0x2c, 0x11, 0x49, 0xfb, 0xa1, 0xc0,
  0xae, 0x6e, 0x30, 0xf8, 0x10, 0x61, 0x9f, 0x53, 0xc6, 0xeb, 0x7c, 0x7c,
  0x94, 0x5f, 0xca, 0xe5, 0x41, 0x40, 0xd6, 0x83, 0xa8, 0x4a, 0x45, 0x5c,
  0x77, 0x69, 0xf2, 0x89, 0xe9, 0xf7, 0x81, 0xa1, 0xae, 0x77, 0xf5, 0xec,
  0x41, 0x4b, 0xfc, 0x82, 0x7f, 0xee, 0x4e, 0x5b, 0x43, 0xdd, 0xcc, 0xe8,
  0x90, 0x12, 0xf0, 0x9a, 0xd0, 0xbc, 0x56, 0xd8, 0xeb, 0xd6, 0xfc, 0x80,
  0x48, 0x26, 0x8c, 0x63, 0x5c, 0x55, 0xd9, 0x6c, 0xb5, 0x10, 0xc1, 0xab,
  0x98, 0x3c, 0xe1, 0x25, 0x13, 0x4e, 0xc2, 0xdc, 0xd5, 0x4a, 0x25, 0xe5,
  0xc7, 0x9f, 0xfc, 0xf8, 0x53, 0x53, 0x12, 0x4c, 0xc1, 0x6c, 0x67, 0xa9,
  0x9e, 0x6e, 0x88, 0x2e, 0x93, 0xa3, 0x3f, 0x2d, 0xd3, 0xc5, 0xf4, 0x25,
  0xe2, 0x0f, 0x8f, 0xc0, 0x2b, 0x30, 0x94, 0x9d, 0xc0, 0x09, 0x31, 0x6c,
  0x19, 0xf7, 0x2e, 0x72, 0x6b, 0xb3, 0xf6, 0x78, 0x91, 0xca, 0x95, 0x28,
  0x9f, 0xf6, 0xd0, 0x77, 0x1c, 0x73, 0xff, 0x30, 0xe7, 0x02, 0xc3, 0xc8,
  0x0e, 0x95, 0x4f, 0x02, 0x96, 0xc5, 0xa9, 0x02, 0xb8, 0xaa, 0x97, 0xac,
  0x37, 0x47, 0xa9, 0x6d, 0xcb, 0x25, 0xb3, 0x8d, 0x20, 0xc5, 0xb6, 0x49,
  0xd8, 0xaf, 0x1a, 0xad, 0x62, 0x8c, 0x79, 0x60, 0x73, 0x8d, 0x46, 0x1e,
  0xe5, 0x23, 0x39, 0x44};

static UCHAR sha256_hash[] = {
  0x27, 0x51, 0x8b, 0xa9, 0x68, 0x30, 0x11, 0xf6, 0xb3, 0x96, 0x07, 0x2c,
  0x05, 0xf6, 0x65, 0x6d, 0x04, 0xf5, 0xfb, 0xc3, 0x78, 0x7c, 0xf9, 0x24,
  0x90, 0xec, 0x60, 0x6e, 0x50, 0x92, 0xe3, 0x26};

static void test_verify_signature(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    BCRYPT_PKCS1_PADDING_INFO padinfo, invalid_padinfo;
    SECURITY_STATUS ret;

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptImportKey(prov, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, &key, signature_pubkey_blob,
                          sizeof(signature_pubkey_blob), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    padinfo.pszAlgId = BCRYPT_SHA256_ALGORITHM;
    ret = NCryptVerifySignature(key, &padinfo, sha256_hash, sizeof(sha256_hash), signature_pkcs1_sha256,
                                sizeof(signature_pkcs1_sha256), NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

    ret = NCryptVerifySignature(key, &padinfo, sha256_hash, sizeof(sha256_hash), invalid_signature,
                                sizeof(invalid_signature), NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_BAD_SIGNATURE, "got %#lx\n", ret);

    ret = NCryptVerifySignature(key, NULL, sha256_hash, sizeof(sha256_hash), signature_pkcs1_sha256,
                                sizeof(signature_pkcs1_sha256), 0);
    ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);

    ret = NCryptVerifySignature(0, &padinfo, sha256_hash, sizeof(sha256_hash), signature_pkcs1_sha256,
                                sizeof(signature_pkcs1_sha256), NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    ret = NCryptVerifySignature(key, &padinfo, NULL, sizeof(sha256_hash), signature_pkcs1_sha256,
                                sizeof(signature_pkcs1_sha256), NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER) || broken(ret == NTE_FAIL) /* win7 */, "got %#lx\n", ret);

    ret = NCryptVerifySignature(key, &padinfo, sha256_hash, 4, signature_pkcs1_sha256,
                                sizeof(signature_pkcs1_sha256), NCRYPT_PAD_PKCS1_FLAG);
    todo_wine ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);

    ret = NCryptVerifySignature(key, &padinfo, sha256_hash, sizeof(sha256_hash), NULL,
                                sizeof(signature_pkcs1_sha256), NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER) || broken(ret == NTE_FAIL) /* win7 */, "got %#lx\n", ret);

    ret = NCryptVerifySignature(key, &padinfo, sha256_hash, sizeof(sha256_hash), signature_pkcs1_sha256, 4,
                                NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);

    invalid_padinfo.pszAlgId = BCRYPT_MD5_ALGORITHM;
    ret = NCryptVerifySignature(key, &invalid_padinfo, sha256_hash, sizeof(sha256_hash), signature_pkcs1_sha256,
                                sizeof(signature_pkcs1_sha256), NCRYPT_PAD_PKCS1_FLAG);
    todo_wine ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);

    NCryptFreeObject(key);
    NCryptFreeObject(prov);
}

static void test_NCryptIsAlgSupported(void)
{
    NCRYPT_PROV_HANDLE prov;
    SECURITY_STATUS ret;

    NCryptOpenStorageProvider(&prov, NULL, 0);
    ret = NCryptIsAlgSupported(0, BCRYPT_RSA_ALGORITHM, 0);
    ok(ret == NTE_INVALID_HANDLE, "expected NTE_INVALID_HANDLE, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, NULL, 0);
    ok(ret == HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER) || broken(ret == NTE_FAIL) /* win7 */, "got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_RSA_ALGORITHM, 20);
    ok(ret == NTE_BAD_FLAGS, "expected NTE_BAD_FLAGS, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_RSA_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS, "expected BCRYPT_RSA_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_RSA_ALGORITHM, NCRYPT_SILENT_FLAG);
    ok(ret == ERROR_SUCCESS, "expected BCRYPT_RSA_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_3DES_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS || broken(ret == NTE_NOT_SUPPORTED) /* win7 */,
       "expected BCRYPT_3DES_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_AES_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS || broken(ret == NTE_NOT_SUPPORTED) /* win7 */,
       "expected BCRYPT_AES_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_ECDH_P256_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS, "expected BCRYPT_ECDH_P256_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_ECDSA_P256_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS, "expected BCRYPT_ECDSA_P256_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_ECDSA_P384_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS, "expected BCRYPT_ECDSA_P384_ALGORITHM to be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_DSA_ALGORITHM, 0);
    ok(ret == ERROR_SUCCESS, "expected BCRYPT_DSA_ALGORITHM to be supported, got %#lx\n", ret);

    /* Not supported */
    ret = NCryptIsAlgSupported(prov, BCRYPT_SHA256_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_SHA256_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_SHA384_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_SHA384_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_SHA512_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_SHA512_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_SHA1_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_SHA1_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_MD5_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_MD5_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_MD4_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_MD4_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_MD2_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_MD2_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_RSA_SIGN_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_RSA_SIGN_ALGORITHM to not be supported, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_RSA_SIGN_ALGORITHM, 20);
    ok(ret == NTE_BAD_FLAGS, "expected NTE_BAD_FLAGS, got %#lx\n", ret);

    ret = NCryptIsAlgSupported(prov, BCRYPT_RNG_ALGORITHM, 0);
    ok(ret == NTE_NOT_SUPPORTED, "expected BCRYPT_RNG_ALGORITHM to not be supported, got %#lx\n", ret);
    NCryptFreeObject(prov);
}

static UCHAR data_to_encrypt[12] = "Hello world";

static void test_NCryptEncrypt(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    BYTE *output_a;
    BYTE *output_b;
    DWORD output_size;

    NCryptOpenStorageProvider(&prov, NULL, 0);
    NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);

    /* Test encrypt with invalid key handle */
    ret = NCryptEncrypt(prov, data_to_encrypt, sizeof(data_to_encrypt), NULL, NULL, 0,
                        &output_size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_INVALID_HANDLE, "got %lx\n", ret);

    /* Test encrypt with a non finalized key */
    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, NULL, 0,
                        &output_size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_BAD_KEY_STATE, "got %lx\n", ret);

    NCryptFinalizeKey(key, 0);

    /* Test encrypt with invalid flags */
    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, NULL, 0,
                        &output_size, 51342);
    ok(ret == NTE_BAD_FLAGS, "got %lx\n", ret);

    /* Test no padding with RSA */
    todo_wine
    {
    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, NULL, 0, &output_size,
                        NCRYPT_NO_PADDING_FLAG);
    ok(ret == ERROR_SUCCESS, "got %lx\n", ret);
    ok(output_size == 128, "got %ld\n", output_size);

    output_a = malloc(output_size);
    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, output_a, output_size,
                        &output_size, NCRYPT_NO_PADDING_FLAG);
    ok(ret == NTE_INVALID_PARAMETER, "got %lx\n", ret);
    free(output_a);
    }

    /* Test output RSA with PKCS1. PKCS1 should append a random padding to the data, so the output should be different
     * with each call. */
    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, NULL, 0,
                        &output_size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %lx\n", ret);
    ok(output_size == 128, "got %ld\n", output_size);

    output_a = malloc(output_size);
    output_b = malloc(output_size);

    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, output_a, 12,
                        &output_size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_BUFFER_TOO_SMALL, "got %lx\n", ret);

    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, output_a, output_size,
                        &output_size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %lx\n", ret);

    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, output_b, output_size,
                        &output_size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %lx\n", ret);
    ok(memcmp(output_a, output_b, 128), "expected to have different outputs.\n");

    NCryptFreeObject(key);
    free(output_a);
    free(output_b);

    NCryptFreeObject(prov);
}

static void test_NCryptExportKey(void)
{
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    DWORD size, policy;
    BYTE buffer[1024];

    ret = NCryptOpenStorageProvider(&prov, NULL, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    NCryptFinalizeKey(key, 0);

    size = 0;
    ret = NCryptExportKey(key, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, buffer, sizeof(buffer), &size, 0);
    ok(ret == ERROR_SUCCESS, "got unexpected return value %lx\n", ret);
    ok(size == 283, "got unexpected size %#lx\n", size);

    size = 0;
    ret = NCryptExportKey(key, 0, BCRYPT_RSAFULLPRIVATE_BLOB, NULL, buffer, sizeof(buffer), &size, 0);
    ok(ret == ERROR_SUCCESS, "got unexpected return value %lx\n", ret);
    ok(size == 603, "got unexpected size %#lx\n", size);

    NCryptFreeObject(key);
    NCryptFreeObject(prov);
}

static void check_key_dword(NCRYPT_HANDLE key, const WCHAR *name, DWORD expected)
{
    DWORD value = 0xdeadbeef, size = 0;
    SECURITY_STATUS ret;

    ret = NCryptGetProperty(key, name, (BYTE *)&value, sizeof(value), &size, 0);
    ok(ret == ERROR_SUCCESS, "%s: got %#lx\n", wine_dbgstr_w(name), ret);
    ok(size == sizeof(DWORD) && value == expected, "%s: got %lu, size %lu, expected %lu\n", wine_dbgstr_w(name),
       value, size, expected);
}

static void check_key_string(NCRYPT_HANDLE key, const WCHAR *name, const WCHAR *expected)
{
    WCHAR value[64];
    DWORD size = 0;
    SECURITY_STATUS ret;

    ret = NCryptGetProperty(key, name, (BYTE *)value, sizeof(value), &size, 0);
    ok(ret == ERROR_SUCCESS, "%s: got %#lx\n", wine_dbgstr_w(name), ret);
    ok(size == (lstrlenW(expected) + 1) * sizeof(WCHAR) && !lstrcmpW(value, expected), "%s: got %s\n",
       wine_dbgstr_w(name), ret ? "" : wine_dbgstr_w(value));
}

static void check_sign_verify(NCRYPT_KEY_HANDLE key, BOOL rsa, DWORD expected_size)
{
    BCRYPT_PKCS1_PADDING_INFO pkcs1 = {BCRYPT_SHA256_ALGORITHM};
    BCRYPT_PSS_PADDING_INFO pss = {BCRYPT_SHA256_ALGORITHM, 32};
    BYTE sig[1024];
    DWORD size = 0;
    SECURITY_STATUS ret;

    ret = NCryptSignHash(key, rsa ? &pkcs1 : NULL, sha256_hash, sizeof(sha256_hash), NULL, 0, &size,
                         rsa ? NCRYPT_PAD_PKCS1_FLAG : 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(size == expected_size, "got size %lu, expected %lu\n", size, expected_size);
    ret = NCryptSignHash(key, rsa ? &pkcs1 : NULL, sha256_hash, sizeof(sha256_hash), sig, sizeof(sig), &size,
                         rsa ? NCRYPT_PAD_PKCS1_FLAG : 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptVerifySignature(key, rsa ? &pkcs1 : NULL, sha256_hash, sizeof(sha256_hash), sig, size,
                                rsa ? NCRYPT_PAD_PKCS1_FLAG : 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    sig[0] ^= 1;
    ret = NCryptVerifySignature(key, rsa ? &pkcs1 : NULL, sha256_hash, sizeof(sha256_hash), sig, size,
                                rsa ? NCRYPT_PAD_PKCS1_FLAG : 0);
    ok(ret == NTE_BAD_SIGNATURE, "got %#lx\n", ret);
    if (!rsa) return;
    ret = NCryptSignHash(key, &pss, sha256_hash, sizeof(sha256_hash), sig, sizeof(sig), &size, NCRYPT_PAD_PSS_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptVerifySignature(key, &pss, sha256_hash, sizeof(sha256_hash), sig, size, NCRYPT_PAD_PSS_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
}

static void test_NCryptDecrypt(void)
{
    BCRYPT_OAEP_PADDING_INFO oaep = {BCRYPT_SHA256_ALGORITHM, NULL, 0};
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    BYTE encrypted[256], decrypted[256];
    DWORD size, size2;

    NCryptOpenStorageProvider(&prov, NULL, 0);
    NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    ret = NCryptDecrypt(key, encrypted, 128, NULL, decrypted, sizeof(decrypted), &size, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);
    NCryptFinalizeKey(key, 0);

    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), NULL, encrypted, sizeof(encrypted), &size,
                        NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    size2 = 0;
    ret = NCryptDecrypt(key, encrypted, size, NULL, NULL, 0, &size2, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(size2 >= sizeof(data_to_encrypt), "got size %lu\n", size2);
    ret = NCryptDecrypt(key, encrypted, size, NULL, decrypted, sizeof(decrypted), &size2, NCRYPT_PAD_PKCS1_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(size2 == sizeof(data_to_encrypt) && !memcmp(decrypted, data_to_encrypt, size2), "wrong data, size %lu\n", size2);

    ret = NCryptEncrypt(key, data_to_encrypt, sizeof(data_to_encrypt), &oaep, encrypted, sizeof(encrypted), &size,
                        NCRYPT_PAD_OAEP_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptDecrypt(key, encrypted, size, &oaep, decrypted, sizeof(decrypted), &size2, NCRYPT_PAD_OAEP_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(size2 == sizeof(data_to_encrypt) && !memcmp(decrypted, data_to_encrypt, size2), "wrong data, size %lu\n", size2);

    ret = NCryptDecrypt(prov, encrypted, size, &oaep, decrypted, sizeof(decrypted), &size2, NCRYPT_PAD_OAEP_FLAG);
    ok(ret == NTE_INVALID_HANDLE, "got %#lx\n", ret);

    NCryptFreeObject(key);
    NCryptFreeObject(prov);
}

static void test_rsa_import_export(void)
{
    static BYTE blob[2048], pkcs8[2048];
    DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG, size, pkcs8_size;
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key, imported;
    SECURITY_STATUS ret;
    BYTE value;

    NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
    ok(NCryptIsKeyHandle(prov) == FALSE, "provider is a key handle\n");
    check_key_string(prov, NCRYPT_NAME_PROPERTY, MS_KEY_STORAGE_PROVIDER);
    check_key_dword(prov, NCRYPT_IMPL_TYPE_PROPERTY, NCRYPT_IMPL_SOFTWARE_FLAG);

    NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, NULL, 0, 0);
    size = 2048;
    NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, (BYTE *)&size, sizeof(size), 0);
    NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
    ret = NCryptFinalizeKey(key, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(NCryptIsKeyHandle(key) == TRUE, "not a key handle\n");
    check_key_dword(key, NCRYPT_LENGTH_PROPERTY, 2048);
    check_key_dword(key, NCRYPT_BLOCK_LENGTH_PROPERTY, 256);
    check_key_dword(key, BCRYPT_SIGNATURE_LENGTH, 256);
    check_sign_verify(key, TRUE, 256);

    /* what RSACng.ImportParameters does */
    ret = NCryptExportKey(key, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, blob, sizeof(blob), &size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, &imported, blob, size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_key_dword(imported, NCRYPT_LENGTH_PROPERTY, 2048);
    check_key_dword(imported, NCRYPT_EXPORT_POLICY_PROPERTY, 0);
    ret = NCryptSetProperty(imported, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptExportKey(imported, 0, BCRYPT_RSAFULLPRIVATE_BLOB, NULL, NULL, 0, &size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_sign_verify(imported, TRUE, 256);

    /* the custom properties of .NET */
    value = 1;
    ret = NCryptSetProperty(imported, L"CLR IsEphemeral", &value, 1, NCRYPT_PERSIST_ONLY_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    value = 0;
    size = 0;
    ret = NCryptGetProperty(imported, L"CLR IsEphemeral", &value, 1, &size, NCRYPT_PERSIST_ONLY_FLAG);
    ok(ret == ERROR_SUCCESS && size == 1 && value == 1, "got %#lx, size %lu, value %u\n", ret, size, value);
    ret = NCryptGetProperty(imported, L"CLR IsEphemeral", &value, 1, &size, 0);
    ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);
    NCryptFreeObject(imported);

    /* PKCS #8 */
    pkcs8_size = 0;
    ret = NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, NULL, 0, &pkcs8_size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(pkcs8_size > 1200 && pkcs8_size < 1230, "got size %lu\n", pkcs8_size);
    ret = NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, pkcs8, 16, &size, 0);
    ok(ret == HRESULT_FROM_WIN32(ERROR_MORE_DATA), "got %#lx\n", ret);
    ret = NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, pkcs8, sizeof(pkcs8), &pkcs8_size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ok(pkcs8[0] == 0x30 && pkcs8[1] == 0x82, "got %02x %02x\n", pkcs8[0], pkcs8[1]);
    ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &imported, pkcs8, pkcs8_size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_key_string(imported, NCRYPT_ALGORITHM_GROUP_PROPERTY, NCRYPT_RSA_ALGORITHM_GROUP);
    check_key_dword(imported, NCRYPT_LENGTH_PROPERTY, 2048);
    check_sign_verify(imported, TRUE, 256);
    NCryptFreeObject(imported);
    ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &imported, pkcs8, pkcs8_size - 1, 0);
    ok(ret == CRYPT_E_BAD_ENCODE, "got %#lx\n", ret);

    /* an imported key that is not finalized yet */
    ret = NCryptExportKey(key, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, blob, sizeof(blob), &size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptImportKey(prov, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, &imported, blob, size, NCRYPT_DO_NOT_FINALIZE_FLAG);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptSetProperty(imported, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptFinalizeKey(imported, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptExportKey(imported, 0, BCRYPT_RSAFULLPRIVATE_BLOB, NULL, NULL, 0, &size, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    NCryptFreeObject(imported);

    NCryptFreeObject(key);
    NCryptFreeObject(prov);
}

static void test_ecc_keys(void)
{
    static const struct
    {
        const WCHAR *alg;
        const WCHAR *group;
        DWORD bits;
        DWORD sig_size;
        DWORD blob_size;
    }
    tests[] =
    {
        { BCRYPT_ECDSA_P256_ALGORITHM, NCRYPT_ECDSA_ALGORITHM_GROUP, 256, 64, 104 },
        { BCRYPT_ECDSA_P384_ALGORITHM, NCRYPT_ECDSA_ALGORITHM_GROUP, 384, 96, 152 },
        { BCRYPT_ECDSA_P521_ALGORITHM, NCRYPT_ECDSA_ALGORITHM_GROUP, 521, 132, 206 },
        { BCRYPT_ECDH_P256_ALGORITHM, NCRYPT_ECDH_ALGORITHM_GROUP, 256, 0, 104 },
    };
    static BYTE blob[512], pkcs8[512];
    DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG, size, pkcs8_size;
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key, imported;
    NCRYPT_SUPPORTED_LENGTHS lengths;
    SECURITY_STATUS ret;
    unsigned int i;

    NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        winetest_push_context("%s", wine_dbgstr_w(tests[i].alg));
        ret = NCryptCreatePersistedKey(prov, &key, tests[i].alg, NULL, 0, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        if (ret)
        {
            winetest_pop_context();
            continue;
        }
        NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ret = NCryptFinalizeKey(key, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        check_key_string(key, NCRYPT_ALGORITHM_PROPERTY, tests[i].alg);
        check_key_string(key, NCRYPT_ALGORITHM_GROUP_PROPERTY, tests[i].group);
        check_key_dword(key, NCRYPT_LENGTH_PROPERTY, tests[i].bits);
        check_key_dword(key, BCRYPT_PUBLIC_KEY_LENGTH, tests[i].bits);
        ret = NCryptGetProperty(key, NCRYPT_LENGTHS_PROPERTY, (BYTE *)&lengths, sizeof(lengths), &size, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        ok(lengths.dwMinLength == tests[i].bits && lengths.dwMaxLength == tests[i].bits && !lengths.dwIncrement &&
           lengths.dwDefaultLength == tests[i].bits, "got %lu %lu %lu %lu\n", lengths.dwMinLength,
           lengths.dwMaxLength, lengths.dwIncrement, lengths.dwDefaultLength);
        ret = NCryptGetProperty(key, BCRYPT_ECC_CURVE_NAME, blob, sizeof(blob), &size, 0);
        ok(ret == NTE_NOT_FOUND, "got %#lx\n", ret);
        if (tests[i].sig_size)
        {
            check_key_dword(key, BCRYPT_SIGNATURE_LENGTH, tests[i].sig_size);
            check_sign_verify(key, FALSE, tests[i].sig_size);
        }

        ret = NCryptExportKey(key, 0, BCRYPT_ECCPRIVATE_BLOB, NULL, blob, sizeof(blob), &size, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        ok(size == tests[i].blob_size, "got size %lu\n", size);
        ret = NCryptImportKey(prov, 0, BCRYPT_ECCPRIVATE_BLOB, NULL, &imported, blob, size, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        if (!ret)
        {
            check_key_string(imported, NCRYPT_ALGORITHM_PROPERTY, tests[i].alg);
            check_key_dword(imported, NCRYPT_LENGTH_PROPERTY, tests[i].bits);
            if (tests[i].sig_size) check_sign_verify(imported, FALSE, tests[i].sig_size);
            NCryptFreeObject(imported);
        }

        pkcs8_size = 0;
        ret = NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, pkcs8, sizeof(pkcs8), &pkcs8_size, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &imported, pkcs8, pkcs8_size, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        if (!ret)
        {
            check_key_dword(imported, NCRYPT_LENGTH_PROPERTY, tests[i].bits);
            if (tests[i].sig_size) check_sign_verify(imported, FALSE, tests[i].sig_size);
            NCryptFreeObject(imported);
        }
        NCryptFreeObject(key);
        winetest_pop_context();
    }

    /* the generic algorithm takes the curve from a property */
    ret = NCryptCreatePersistedKey(prov, &key, BCRYPT_ECDSA_ALGORITHM, NULL, 0, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptSetProperty(key, BCRYPT_ECC_CURVE_NAME, (BYTE *)L"nistP384", sizeof(L"nistP384"), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    ret = NCryptFinalizeKey(key, 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    check_key_string(key, NCRYPT_ALGORITHM_PROPERTY, BCRYPT_ECDSA_ALGORITHM);
    check_key_string(key, BCRYPT_ECC_CURVE_NAME, L"nistP384");
    check_key_dword(key, NCRYPT_LENGTH_PROPERTY, 384);
    check_sign_verify(key, FALSE, 96);
    NCryptFreeObject(key);

    NCryptFreeObject(prov);
}

static void check_export(const char *ctx, NCRYPT_KEY_HANDLE key, const WCHAR *type, SECURITY_STATUS expected)
{
    BYTE buffer[1024];
    DWORD size = 0;
    SECURITY_STATUS ret;

    ret = NCryptExportKey(key, 0, type, NULL, NULL, 0, &size, 0);
    ok(ret == expected, "%s %s: got %#lx, expected %#lx\n", ctx, wine_dbgstr_w(type), ret, expected);
    ret = NCryptExportKey(key, 0, type, NULL, buffer, sizeof(buffer), &size, 0);
    ok(ret == expected, "%s %s: got %#lx, expected %#lx\n", ctx, wine_dbgstr_w(type), ret, expected);
}

static void check_exports(const char *ctx, NCRYPT_KEY_HANDLE key, BOOL rsa, BOOL plaintext)
{
    SECURITY_STATUS priv = plaintext ? ERROR_SUCCESS : NTE_NOT_SUPPORTED;

    check_export(ctx, key, BCRYPT_PUBLIC_KEY_BLOB, ERROR_SUCCESS);
    check_export(ctx, key, rsa ? BCRYPT_RSAPUBLIC_BLOB : BCRYPT_ECCPUBLIC_BLOB, ERROR_SUCCESS);
    check_export(ctx, key, rsa ? BCRYPT_ECCPUBLIC_BLOB : BCRYPT_RSAPUBLIC_BLOB, NTE_BAD_TYPE);
    check_export(ctx, key, BCRYPT_PRIVATE_KEY_BLOB, priv);
    check_export(ctx, key, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, priv);
    check_export(ctx, key, rsa ? BCRYPT_RSAPRIVATE_BLOB : BCRYPT_ECCPRIVATE_BLOB, priv);
    check_export(ctx, key, rsa ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_RSAPRIVATE_BLOB, NTE_BAD_TYPE);
    if (rsa) check_export(ctx, key, BCRYPT_RSAFULLPRIVATE_BLOB, priv);
}

static void test_export_policy(void)
{
    static const WCHAR *algs[] = { BCRYPT_RSA_ALGORITHM, BCRYPT_ECDSA_P256_ALGORITHM };
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    DWORD policy, i;

    NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
    for (i = 0; i < ARRAY_SIZE(algs); i++)
    {
        BOOL rsa = i == 0;

        winetest_push_context("%s", wine_dbgstr_w(algs[i]));
        ret = NCryptCreatePersistedKey(prov, &key, algs[i], NULL, 0, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        check_key_dword(key, NCRYPT_EXPORT_POLICY_PROPERTY, 0);
        check_export("unfinalized", key, BCRYPT_PUBLIC_KEY_BLOB, NTE_BAD_KEY_STATE);
        check_export("unfinalized", key, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NTE_BAD_KEY_STATE);

        policy = NCRYPT_ALLOW_EXPORT_FLAG;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        ret = NCryptFinalizeKey(key, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);

        /* the right to export is not the right to export in plaintext */
        check_exports("allow export", key, rsa, FALSE);

        /* the policy can be changed after the key is finalized, and is read when the key is exported */
        policy = NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        check_key_dword(key, NCRYPT_EXPORT_POLICY_PROPERTY, NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG);
        check_exports("allow plaintext export", key, rsa, TRUE);
        policy = 0;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        check_exports("no policy", key, rsa, FALSE);

        /* archiving implies exporting */
        policy = NCRYPT_ALLOW_ARCHIVING_FLAG;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        check_key_dword(key, NCRYPT_EXPORT_POLICY_PROPERTY, NCRYPT_ALLOW_ARCHIVING_FLAG | NCRYPT_ALLOW_EXPORT_FLAG);
        check_exports("archiving", key, rsa, FALSE);
        policy = NCRYPT_ALLOW_PLAINTEXT_ARCHIVING_FLAG;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        check_key_dword(key, NCRYPT_EXPORT_POLICY_PROPERTY,
                        NCRYPT_ALLOW_PLAINTEXT_ARCHIVING_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG);
        check_exports("plaintext archiving", key, rsa, TRUE);

        /* invalid values leave the policy alone */
        policy = 0x10;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);
        policy = 0xffffffff;
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, sizeof(policy), 0);
        ok(ret == NTE_NOT_SUPPORTED, "got %#lx\n", ret);
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, 2, 0);
        ok(ret == NTE_BAD_DATA, "got %#lx\n", ret);
        ret = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (BYTE *)&policy, 8, 0);
        ok(ret == NTE_BAD_DATA, "got %#lx\n", ret);
        check_key_dword(key, NCRYPT_EXPORT_POLICY_PROPERTY,
                        NCRYPT_ALLOW_PLAINTEXT_ARCHIVING_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG);
        NCryptFreeObject(key);
        winetest_pop_context();
    }
    NCryptFreeObject(prov);
}

static void test_pkcs8_ec_without_public_key(void)
{
    /* PrivateKeyInfo of a P-256 key with d = 2, the ECPrivateKey has neither parameters nor a public key */
    static const BYTE pkcs8[] =
    {
        0x30, 0x41, 0x02, 0x01, 0x00, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x04, 0x27, 0x30, 0x25, 0x02, 0x01,
        0x01, 0x04, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 2,
    };
    /* 2G */
    static const BYTE x[] = {0x7c,0xf2,0x7b,0x18,0x8d,0x03,0x4f,0x7e,0x8a,0x52,0x38,0x03,0x04,0xb5,0x1a,0xc3,
                             0xc0,0x89,0x69,0xe2,0x77,0xf2,0x1b,0x35,0xa6,0x0b,0x48,0xfc,0x47,0x66,0x99,0x78};
    static const BYTE y[] = {0x07,0x77,0x55,0x10,0xdb,0x8e,0xd0,0x40,0x29,0x3d,0x9a,0xc6,0x9f,0x74,0x30,0xdb,
                             0xba,0x7d,0xad,0xe6,0x3c,0xe9,0x82,0x29,0x9e,0x04,0xb7,0x9d,0x22,0x78,0x73,0xd1};
    BYTE data[sizeof(pkcs8) + 1], blob[256];
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    SECURITY_STATUS ret;
    DWORD size;

    NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
    ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &key, (BYTE *)pkcs8, sizeof(pkcs8), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    if (!ret)
    {
        check_key_dword(key, NCRYPT_LENGTH_PROPERTY, 256);
        ret = NCryptExportKey(key, 0, BCRYPT_PUBLIC_KEY_BLOB, NULL, blob, sizeof(blob), &size, 0);
        ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
        ok(size == sizeof(BCRYPT_ECCKEY_BLOB) + 64, "got size %lu\n", size);
        ok(!memcmp(blob + sizeof(BCRYPT_ECCKEY_BLOB), x, 32) && !memcmp(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, y, 32),
           "the public key was not derived\n");
        NCryptFreeObject(key);
    }

    /* data after the key is ignored */
    memcpy(data, pkcs8, sizeof(pkcs8));
    data[sizeof(pkcs8)] = 0;
    ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &key, data, sizeof(data), 0);
    ok(ret == ERROR_SUCCESS, "got %#lx\n", ret);
    if (!ret) NCryptFreeObject(key);

    /* a version other than 0 is not accepted for an EC key */
    memcpy(data, pkcs8, sizeof(pkcs8));
    data[6] = 1;
    ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &key, data, sizeof(pkcs8), 0);
    ok(ret == CRYPT_E_BAD_ENCODE, "got %#lx\n", ret);
    if (!ret) NCryptFreeObject(key);

    /* an invalid private key */
    memcpy(data, pkcs8, sizeof(pkcs8));
    data[sizeof(pkcs8) - 1] = 0;
    ret = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, &key, data, sizeof(pkcs8), 0);
    ok(ret == NTE_INVALID_PARAMETER, "got %#lx\n", ret);
    if (!ret) NCryptFreeObject(key);
    NCryptFreeObject(prov);
}

START_TEST(ncrypt)
{
    test_key_import_rsa();
    test_free_object();
    test_get_property();
    test_set_property();
    test_create_persisted_key();
    test_finalize_key();
    test_verify_signature();
    test_NCryptIsAlgSupported();
    test_NCryptEncrypt();
    test_NCryptExportKey();
    test_NCryptDecrypt();
    test_rsa_import_export();
    test_ecc_keys();
    test_export_policy();
    test_pkcs8_ec_without_public_key();
}
