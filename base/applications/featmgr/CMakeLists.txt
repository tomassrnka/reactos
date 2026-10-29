
project(featmgr)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# --- CLI frontend (automation): featmgr /list /query /enable /disable ---
add_executable(featmgr cli.c engine.c rapps_import.c featmgr.rc)
set_module_type(featmgr win32cui UNICODE)
add_importlibs(featmgr advapi32 shlwapi shell32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET featmgr DESTINATION reactos/system32 FOR all)

# --- GUI applet: "Optional Features" (Turn features on/off) ---
add_executable(optfeatures gui.c engine.c rapps_import.c optfeatures.rc)
set_module_type(optfeatures win32gui UNICODE)
add_importlibs(optfeatures advapi32 shlwapi shell32 comctl32 user32 gdi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET optfeatures DESTINATION reactos/system32 FOR all)

# --- ship the feature catalog + proof-feature payload into the image ---
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/gl-sw-pack.ini
            DESTINATION reactos/Featmgr/Catalog FOR all)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/lavapipe-pack.ini
            DESTINATION reactos/Featmgr/Catalog FOR all)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/winfsp.ini
            DESTINATION reactos/Featmgr/Catalog FOR all)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/payload/gl-sw-pack/glsw-pack.txt
            DESTINATION reactos/Featmgr/Payload/gl-sw-pack FOR all)

# --- Claude Desktop: software-tier entry (amd64 only, Anthropic ships an x64 build) ---
# Only the manifest and a RAPPS-format entry (not a manifest: the catalog scan reads *.ini) ship in the image. The app itself is downloaded
# from Anthropic's release feed at install time (SHA1-checked) and is never bundled.
if(ARCH STREQUAL "amd64")
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/claude-desktop.ini
                DESTINATION reactos/Featmgr/Catalog FOR all)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/claude-desktop.txt
                DESTINATION reactos/Featmgr/Catalog FOR all)
endif()

# --- PowerShell 7 (LTS): software-tier entry (amd64 only; the x86 pwsh does not start yet, PS7-x86-pwsh-mscoree) ---
# Manifest + RAPPS-format entry only. The official MSI is downloaded from the PowerShell GitHub release at
# install time (SHA-256 checked) and is never bundled.
if(ARCH STREQUAL "amd64")
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/powershell-7.ini
                DESTINATION reactos/Featmgr/Catalog FOR all)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/catalog/powershell-7.txt
                DESTINATION reactos/Featmgr/Catalog FOR all)
endif()
