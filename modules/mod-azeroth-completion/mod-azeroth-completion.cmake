# CMake hooks for mod-azeroth-completion.
#
# Sources under src/ and conf/*.conf.dist are picked up automatically, and
# Addmod_azeroth_completionScripts() is called by the generated module loader.

# The core does not install module SQL, and a build whose TW_SOURCE_MODULES_DIR points at the
# install prefix (as the tortoise-deploy images do) would otherwise never create the tables.
if(TORTOISE_MODULE_CMAKE_PHASE STREQUAL "POST_TARGETS")
  install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/data/sql"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/modules/mod-azeroth-completion/data"
    FILES_MATCHING PATTERN "*.sql")
endif()
