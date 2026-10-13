#ifndef _DISKMGMT_RESOURCE_H_
#define _DISKMGMT_RESOURCE_H_

#define IDI_DISKMGMT            1
#define IDR_MAINMENU            101
#define IDR_ACCEL               102
#define IDR_POPUP               103
#define IDB_PROP                110
#define IDB_REFRESH             111

/* commands */
#define ID_EXIT                 200
#define ID_REFRESH              201
#define ID_PROPERTIES           202
#define ID_TOP_DISKLIST         210
#define ID_TOP_VOLLIST          211
#define ID_TOP_GRAPH            212
#define ID_BOT_DISKLIST         220
#define ID_BOT_VOLLIST          221
#define ID_BOT_GRAPH            222
#define ID_BOT_HIDDEN           223
#define ID_ABOUT                230

/* control ids of the main window */
#define IDC_TOOLBAR             300
#define IDC_STATUSBAR           301
#define IDC_DISKLIST            302
#define IDC_VOLLIST             303
#define IDC_DISKLIST2           304
#define IDC_VOLLIST2            305
#define IDC_GRAPH               306
#define IDC_GRAPH2              307

/* property pages */
#define IDD_VOL_GENERAL         400
#define IDD_VOL_PARTITION       401
#define IDD_DISK_GENERAL        402
#define IDD_DISK_VOLUMES        403

#define IDC_VOL_ICON            410
#define IDC_VOL_NAME            411
#define IDC_VOL_TYPE            412
#define IDC_VOL_FS              413
#define IDC_VOL_USED_BYTES      414
#define IDC_VOL_USED_HUMAN      415
#define IDC_VOL_FREE_BYTES      416
#define IDC_VOL_FREE_HUMAN      417
#define IDC_VOL_CAP_BYTES       418
#define IDC_VOL_CAP_HUMAN       419
#define IDC_VOL_PIE             420
#define IDC_VOL_DRIVE           421
#define IDC_VOL_USED_COLOR      422
#define IDC_VOL_FREE_COLOR      423
#define IDC_VOL_NOTE            424
#define IDC_VOL_USED_LABEL      425
#define IDC_VOL_FREE_LABEL      426

#define IDC_PROP_LIST           430

#define IDC_DISK_ICON           440
#define IDC_DISK_NAME           441
#define IDC_DISK_DEVTYPE        442
#define IDC_DISK_LOCATION       443
#define IDC_DISK_STATUS         444

#define IDC_DV_DISK             450
#define IDC_DV_TYPE             451
#define IDC_DV_STATUS           452
#define IDC_DV_STYLE            453
#define IDC_DV_CAPACITY         454
#define IDC_DV_UNALLOC          455
#define IDC_DV_RESERVED         456
#define IDC_DV_LIST             457
#define IDC_DV_PROPERTIES       458

/* strings */
#define IDS_APPTITLE            500
#define IDS_COL_VOLUME          501
#define IDS_COL_LAYOUT          502
#define IDS_COL_TYPE            503
#define IDS_COL_FS              504
#define IDS_COL_STATUS          505
#define IDS_COL_CAPACITY        506
#define IDS_COL_FREE            507
#define IDS_COL_PCTFREE         508
#define IDS_COL_FAULT           509
#define IDS_COL_OVERHEAD        510
#define IDS_COL_DISK            511
#define IDS_COL_UNALLOC         512
#define IDS_COL_DEVTYPE         513
#define IDS_COL_STYLE           514
#define IDS_LEGEND_UNALLOC      520
#define IDS_LEGEND_PRIMARY      521
#define IDS_LEGEND_EXTENDED     522
#define IDS_LEGEND_FREE         523
#define IDS_LEGEND_LOGICAL      524
#define IDS_STATUS_COUNTS       530
#define IDS_ABOUT_TEXT          532
#define IDS_TIP_PROPERTIES      533
#define IDS_TIP_REFRESH         534
#define IDS_STYLE_MBR_SHORT     535
#define IDS_STYLE_GPT_SHORT     536
#define IDS_PROP_VOLUME_TITLE   540
#define IDS_PROP_PARTITION_TITLE 541
#define IDS_PROP_DISK_TITLE     542
#define IDS_PAGE_GENERAL        543
#define IDS_PAGE_PARTITION      544
#define IDS_PAGE_VOLUMES        545
#define IDS_TYPE_LOCAL          546
#define IDS_TYPE_CDROM          547
#define IDS_TYPE_REMOVABLE      548
#define IDS_NOTE_NOFS           549
#define IDS_DRIVE_FMT           550
#define IDS_DEVTYPE_DISK        551
#define IDS_LOCATION_FMT        552
#define IDS_STYLE_MBR           553
#define IDS_STYLE_GPT           554
#define IDS_STYLE_RAW           555
#define IDS_COL_PROPERTY        556
#define IDS_COL_VALUE           557
#define IDS_COL_VOL_NAME        558
#define IDS_COL_VOL_CAP         559
#define IDS_MB_FMT              560
#define IDS_YES                 561
#define IDS_NO                  562
#define IDS_PROP_DISK           563
#define IDS_PROP_PARTNUM        564
#define IDS_PROP_STYLE          565
#define IDS_PROP_TYPE           566
#define IDS_PROP_OFFSET         567
#define IDS_PROP_LENGTH         568
#define IDS_PROP_ACTIVE         569
#define IDS_PROP_VOLGUID        570
#define IDS_PROP_PATHS          571
#define IDS_PROP_PARTID         572
#define IDS_PROP_PARTNAME       573
#define IDS_PROP_STATUS         574
#define IDS_PROP_LABEL          575
#define IDS_BYTES_FMT           576
#define IDS_NOT_AVAILABLE       579

#endif
