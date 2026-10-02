# Optional CMake hooks for mod-thrill-of-the-kill.
#
# Most modules do not need custom CMake here. Source files under src/ are discovered automatically,
# conf/*.conf.dist files are copied automatically, and Addmod_thrill_of_the_killScripts() is invoked by the
# generated module loader.
#
# This file is included twice:
#   TORTOISE_MODULE_CMAKE_PHASE=DISCOVERY
#   TORTOISE_MODULE_CMAKE_PHASE=POST_TARGETS
#
# Use TW_* helpers for compatibility with this core's module system. Do not use AzerothCore AC_* names.

if(TORTOISE_MODULE_CMAKE_PHASE STREQUAL "DISCOVERY")
  # Example for legacy-style extra script sources:
  # TW_ADD_SCRIPTS("${CMAKE_CURRENT_LIST_DIR}/src/legacy")
  # TW_ADD_SCRIPT_LOADER(mod_thrill_of_the_kill "legacy_loader.h")
endif()

# The core does not install module SQL, and a build whose TW_SOURCE_MODULES_DIR points at the
# install prefix (as the tortoise-deploy images do) would otherwise never apply spell 61500.
if(TORTOISE_MODULE_CMAKE_PHASE STREQUAL "POST_TARGETS")
  install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/data/sql"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/modules/mod-thrill-of-the-kill/data"
    FILES_MATCHING PATTERN "*.sql")
endif()
