/*
 * Resource identifiers for ieframe.dll
 *
 * Copyright 2010 Alexander N. Sørnes <alex@thehandofagony.com>
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

#pragma once

#include <windef.h>
#include <winuser.h>

#define IDR_BROWSE_MAIN_MENU           1000
#define IDD_BROWSE_OPEN                1001
#define IDC_BROWSE_OPEN_URL            1002
#define IDC_BROWSE_REBAR               1003
#define IDC_BROWSE_ADDRESSBAR          1004
#define IDC_BROWSE_STATUSBAR           1005
#define IDC_BROWSE_TOOLBAR             1006

#define ID_BROWSE_NEW_WINDOW           275
#define ID_BROWSE_OPEN                 256
#define ID_BROWSE_SAVE                 257
#define ID_BROWSE_SAVE_AS              258
#define ID_BROWSE_PRINT_FORMAT         259
#define ID_BROWSE_PRINT                260
#define ID_BROWSE_PRINT_PREVIEW        277
#define ID_BROWSE_PROPERTIES           262
#define ID_BROWSE_QUIT                 278
#define ID_BROWSE_ABOUT                336

#define ID_BROWSE_ADDFAV               1200
#define ID_BROWSE_HOME                 1201
#define ID_BROWSE_BACK                 1202
#define ID_BROWSE_FORWARD              1203
#define ID_BROWSE_STOP                 1204
#define ID_BROWSE_REFRESH              1205

#define ID_BROWSE_BAR_STD              1300
#define ID_BROWSE_BAR_ADDR             1301

#define ID_BROWSE_GOTOFAV_FIRST        2000
#define ID_BROWSE_GOTOFAV_MAX          65000

#ifdef __REACTOS__
#define IDS_INTERNET                   880
#define IDS_INTERNET_DESCRIPTION       881
#endif

#define IDS_TB_BACK                    1100
#define IDS_TB_FORWARD                 1101
#define IDS_TB_STOP                    1102
#define IDS_TB_REFRESH                 1103
#define IDS_TB_HOME                    1104
#define IDS_TB_PRINT                   1105

#define IDS_ADDRESS                    1106

#define IDB_IETOOLBAR                  1400
#ifdef __REACTOS__
#define IDC_PAGEICO                    1401
#endif

/* update status text in BINDSTATUS_* callback */
#define IDS_STATUSFMT_FIRST            4096
#define IDS_FINDINGRESOURCE            (IDS_STATUSFMT_FIRST + 1)
#define IDS_BEGINDOWNLOADDATA          (IDS_STATUSFMT_FIRST + 4)
#define IDS_ENDDOWNLOADDATA            (IDS_STATUSFMT_FIRST + 6)
#define IDS_SENDINGREQUEST             (IDS_STATUSFMT_FIRST + 11)
#define IDS_STATUSFMT_MAXLEN           256

#ifdef __REACTOS__
/* File download: File Download dialog, progress dialog and their strings */
#define IDD_DOWNLOAD_ASK               1500
#define IDD_DOWNLOAD_PROGRESS          1501

#define IDC_DLA_QUESTION               1510
#define IDC_DLA_ICON                   1511
#define IDC_DLA_NAME                   1512
#define IDC_DLA_TYPE                   1513
#define IDC_DLA_FROM                   1514
#define IDC_DLA_OPEN                   1515
#define IDC_DLA_SAVE                   1516
#define IDC_DLA_WARNICON               1517
#define IDC_DLA_WARNING                1518

#define IDC_DLP_ICON                   1530
#define IDC_DLP_TITLE                  1531
#define IDC_DLP_NAME                   1532
#define IDC_DLP_BAR                    1533
#define IDC_DLP_LABEL1                 1534
#define IDC_DLP_VALUE1                 1535
#define IDC_DLP_VALUE2                 1536
#define IDC_DLP_VALUE3                 1537
#define IDC_DLP_CLOSE                  1538
#define IDC_DLP_OPEN                   1539
#define IDC_DLP_FOLDER                 1540

#define IDS_DL_ASK_TITLE               1600
#define IDS_DL_ASK_TITLE_WARN          1601
#define IDS_DL_Q_RUN                   1602
#define IDS_DL_Q_OPEN                  1603
#define IDS_DL_Q_SAVE                  1604
#define IDS_DL_WARN_APP                1605
#define IDS_DL_WARN_FILE               1606
#define IDS_DL_WARN_UNKNOWN            1607
#define IDS_DL_BTN_RUN                 1608
#define IDS_DL_TYPE_UNKNOWN            1610
#define IDS_DL_TYPE_SIZE               1611
#define IDS_DL_CAPTION_PCT             1612
#define IDS_DL_CAPTION_COPIED          1613
#define IDS_DL_CAPTION_DONE            1614
#define IDS_DL_NAME_FROM               1615
#define IDS_DL_TIMELEFT                1616
#define IDS_DL_TIMELEFT_VAL            1617
#define IDS_DL_TIMELEFT_UNKNOWN        1618
#define IDS_DL_DOWNLOADED              1619
#define IDS_DL_DOWNLOADED_VAL          1620
#define IDS_DL_RATE_VAL                1621
#define IDS_DL_COMPLETE                1622
#define IDS_DL_BTN_CLOSE               1623
#define IDS_DL_STATUS_START            1625
#define IDS_DL_STATUS_PROGRESS         1626
#define IDS_DL_STATUS_DONE             1627
#define IDS_DL_ERR_FMT                 1628
#define IDS_DL_ERR_HTTP                1629
#define IDS_DL_ERR_INTERRUPTED         1630
#define IDS_DL_ERR_NET                 1631
#define IDS_DL_ERR_WRITE               1632
#define IDS_DL_ERR_CREATE              1633
#define IDS_DL_ERR_TITLE               1634
#define IDS_DL_SAVEAS_ALL              1635
#define IDS_DL_UNIT_BYTES              1636
#define IDS_DL_TIME_HMS                1637
#define IDS_DL_TIME_MS                 1638
#define IDS_DL_TIME_S                  1639
#define IDS_DL_ERR_OPEN                1640
#define IDS_DL_TYPE_APP                1641
#endif
