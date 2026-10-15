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

/* actions of the writer */
#define ID_ACT_FIRST            240
#define ID_OPEN                 240
#define ID_EXPLORE              241
#define ID_MARK_ACTIVE          242
#define ID_CHANGE_LETTER        243
#define ID_FORMAT               244
#define ID_DELETE_VOLUME        245
#define ID_DELETE_PARTITION     246
#define ID_NEW_VOLUME           247
#define ID_INIT_DISK            248
#define ID_CONVERT_GPT          249
#define ID_CONVERT_MBR          250
#define ID_RESCAN               251
#define ID_ACT_LAST             251
#define ID_ALL_TASKS            260

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

/* dialogs of the actions */
#define IDD_INITDISK            600
#define IDD_NEWVOL              601
#define IDD_LETTER              602
#define IDD_PICKLETTER          603
#define IDD_FORMAT              604
#define IDD_PROGRESS            605

#define IDC_ID_TEXT             610
#define IDC_ID_MBR              611
#define IDC_ID_GPT              612

#define IDC_NV_INFO             620
#define IDC_NV_SIZE             621
#define IDC_NV_LETTER           622
#define IDC_NV_FS               623
#define IDC_NV_LABEL            624
#define IDC_NV_QUICK            625
#define IDC_NV_NOTE             626

#define IDC_CL_VOLUME           630
#define IDC_CL_LIST             631
#define IDC_CL_ADD              632
#define IDC_CL_CHANGE           633
#define IDC_CL_REMOVE           634

#define IDC_PL_LETTER_RADIO     640
#define IDC_PL_LETTER           641
#define IDC_PL_FOLDER_RADIO     642
#define IDC_PL_FOLDER           643

#define IDC_FM_VOLUME           650
#define IDC_FM_LABEL            651
#define IDC_FM_FS               652
#define IDC_FM_CLUSTER          653
#define IDC_FM_QUICK            654

#define IDC_PR_TEXT             660
#define IDC_PR_BAR              661

/* strings of the actions */
#define IDS_ACT_OPEN            600
#define IDS_ACT_EXPLORE         601
#define IDS_ACT_ACTIVE          602
#define IDS_ACT_LETTER          603
#define IDS_ACT_FORMAT          604
#define IDS_ACT_DELETE_VOLUME   605
#define IDS_ACT_DELETE_PARTITION 606
#define IDS_ACT_NEW             607
#define IDS_ACT_INIT            608
#define IDS_ACT_GPT             609
#define IDS_ACT_MBR             610
#define IDS_ACT_RESCAN          611
#define IDS_ACT_PROPERTIES      612
#define IDS_ACT_ALL_TASKS       613
#define IDS_CONFIRM_TITLE       620
#define IDS_CONFIRM_DELETE      621
#define IDS_CONFIRM_FORMAT      622
#define IDS_CONFIRM_ACTIVE      623
#define IDS_CONFIRM_CONVERT     624
#define IDS_CONFIRM_LETTER      625
#define IDS_CONFIRM_FORCE       626
#define IDS_CONFIRM_REMOVE_PATH 627
#define IDS_ERROR_TITLE         630
#define IDS_ERR_GENERIC         631
#define IDS_ERR_FORMAT          632
#define IDS_ERR_NO_PROVIDER     633
#define IDS_ERR_IN_USE          634
#define IDS_ERR_NOT_EMPTY       635
#define IDS_ERR_LAYOUT          636
#define IDS_ERR_NO_VOLUME       637
#define IDS_ERR_NOT_SUPPORTED   638
#define IDS_ERR_PROTECTED       639
#define IDS_ERR_FORMAT_CODE     640
#define IDS_FORMAT_DONE         641
#define IDS_FORMATTING          642
#define IDS_WORKING             643
#define IDS_ERR_CHANGED         644
#define IDS_ERR_UNREADABLE      645
#define IDS_ERR_TYPE_NOT_UPDATED 646
#define IDS_ERR_VERIFY_FAILED   647
#define IDS_ERR_RESTORE_FAILED  648
#define IDS_NV_INFO_FMT         650
#define IDS_NV_NO_LETTER        651
#define IDS_NV_NO_FORMAT        652
#define IDS_NV_SIZE_RANGE       653
#define IDS_NV_LABEL_DEFAULT    654
#define IDS_NV_NOTE_EXTENDED    655
#define IDS_NV_NOTE_RESERVED    656
#define IDS_NV_NOTE_LOGICAL     657
#define IDS_NV_TITLE            658
#define IDS_NV_CREATED_NO_VOLUME 659
#define IDS_ID_TEXT_FMT         660
#define IDS_FM_CLUSTER_DEFAULT  661
#define IDS_FM_TITLE            662
#define IDS_FM_VOLUME_FMT       663
#define IDS_CL_TITLE            664
#define IDS_PL_BAD_FOLDER       665
#define IDS_PL_NO_LETTER_FREE   666
#define IDS_CL_NOTHING          667
#define IDS_PROP_LABEL_FAILED   668
#define IDS_PL_FOLDER_PATH      669
#define IDS_ID_TITLE            670
#define IDS_PL_NO_REPARSE       671
#define IDS_ERR_PATHS_NOT_RESTORED 890
#define IDS_PL_FOLDER_TOO_LONG  891

#endif
